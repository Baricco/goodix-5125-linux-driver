#!/usr/bin/env python3
"""Mosaic reconstruction of the fingerprint from overlapping scans (Goodix 5125).

Idea: when mapping in motion, adjacent scans have a small but real displacement. The rotation
plus translation between pairs is estimated (coverage.estimate_offset), a pose graph is built,
the scans of every connected component are placed in a common frame (spanning tree from the node
of highest degree) and the images are fused into a composite. The coverage mask reveals the
HOLES: areas inside the envelope of the map not covered by any scan -> they guide the mapping.

Works on a list of (feats, thumb) where feats=(pts,desc) and thumb is the preprocessed (H,W)
uint8 image of that scan. No scaling (same sensor): pure rotation plus translation."""

import numpy as np
import cv2

import coverage

MIN_INLIERS = 7       # minimum RANSAC inliers to trust a link between two scans
H, W = 64, 80


def build_graph(items):
    """items: list of (feats, thumb). -> dict edge (i,j)->(M 2x3 taking j onto i, inliers)."""
    n = len(items)
    edges = {}
    for i in range(n):
        fi = items[i][0]
        for j in range(i + 1, n):
            ok, dx, dy, deg, inl = coverage.estimate_offset(fi, items[j][0])
            if ok and inl >= MIN_INLIERS:
                th = np.radians(deg)
                c, s = np.cos(th), np.sin(th)
                M = np.array([[c, -s, dx], [s, c, dy]], np.float32)   # j -> i
                edges[(i, j)] = (M, inl)
    return edges


def _neighbors(edges, n):
    adj = {k: [] for k in range(n)}
    for (i, j), (M, inl) in edges.items():
        adj[i].append((j, _invert(M), inl))   # going from i to j needs M^-1? see below
        adj[j].append((i, M, inl))
    return adj


def _invert(M):
    R = M[:, :2]
    t = M[:, 2]
    Ri = np.linalg.inv(R)
    return np.hstack([Ri, (-Ri @ t).reshape(2, 1)]).astype(np.float32)


def _compose(A, B):
    """A,B 2x3 affine; returns A∘B (applies B, then A)."""
    Ra, ta = A[:, :2], A[:, 2]
    Rb, tb = B[:, :2], B[:, 2]
    return np.hstack([Ra @ Rb, (Ra @ tb + ta).reshape(2, 1)]).astype(np.float32)


def components(edges, n):
    adj = {k: set() for k in range(n)}
    for (i, j) in edges:
        adj[i].add(j); adj[j].add(i)
    seen, comps = set(), []
    for k in range(n):
        if k in seen:
            continue
        st, comp = [k], []
        while st:
            x = st.pop()
            if x in seen:
                continue
            seen.add(x); comp.append(x); st += [y for y in adj[x] if y not in seen]
        comps.append(sorted(comp))
    return comps


def place_component(comp, edges):
    """Places the scans of a component in common coordinates (tree from the most connected node).
    -> dict node -> M (2x3) taking the node's coordinates into the common frame."""
    # adjacency with the node->neighbour transformation
    adj = {k: [] for k in comp}
    for (i, j), (M, inl) in edges.items():
        if i in adj and j in adj:
            adj[i].append((j, M))       # M: j->i
            adj[j].append((i, _invert(M)))  # i->j
    root = max(comp, key=lambda k: len(adj[k]))
    T = {root: np.array([[1, 0, 0], [0, 1, 0]], np.float32)}
    st = [root]
    while st:
        p = st.pop()
        for (q, M_qp) in adj[p]:        # M_qp: p->q ... we build q->root
            if q in T:
                continue
            # we want q->root. We have p->root (T[p]) and an M linking p and q.
            # adj stores (neighbour, transform p->neighbour). Do we need neighbour->root = T[p] ∘ (p->neighbour)^-1? no:
            # if M_qp takes p onto q (p->q), then q->p = inv(M_qp), and q->root = T[p] ∘ (q->p).
            T[q] = _compose(T[p], _invert(M_qp))
            st.append(q)
    return T, root


def render(items, comp, T, margin=40):
    """Fuses the component's thumbnails into the composite. -> (mosaic u8, coverage count, bbox)."""
    corners = np.array([[0, 0], [W, 0], [W, H], [0, H]], np.float32)
    allpts = []
    for k in comp:
        M = T[k]
        pc = (M[:, :2] @ corners.T).T + M[:, 2]
        allpts.append(pc)
    allpts = np.vstack(allpts)
    x0, y0 = allpts.min(0) - margin
    x1, y1 = allpts.max(0) + margin
    out_w, out_h = int(np.ceil(x1 - x0)), int(np.ceil(y1 - y0))
    shift = np.array([[1, 0, -x0], [0, 1, -y0]], np.float32)
    acc = np.zeros((out_h, out_w), np.float32)
    cnt = np.zeros((out_h, out_w), np.float32)
    for k in comp:
        M = _compose(shift, T[k])
        warp = cv2.warpAffine(items[k][1].astype(np.float32), M, (out_w, out_h))
        mask = cv2.warpAffine(np.ones((H, W), np.float32), M, (out_w, out_h))
        acc += warp * mask
        cnt += mask
    mosaic = np.zeros((out_h, out_w), np.uint8)
    nz = cnt > 0
    mosaic[nz] = np.clip(acc[nz] / cnt[nz], 0, 255).astype(np.uint8)
    return mosaic, cnt, (x0, y0, out_w, out_h)


def holes(cnt):
    """Holes = areas inside the coverage envelope but not covered. -> (mask_holes, fraction)."""
    covered = (cnt > 0).astype(np.uint8)
    # envelope: morphological closing + fill of the outer contours
    k = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (15, 15))
    envelope = cv2.morphologyEx(covered, cv2.MORPH_CLOSE, k)
    cnts, _ = cv2.findContours(envelope, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    filled = np.zeros_like(envelope)
    cv2.drawContours(filled, cnts, -1, 1, -1)
    hole = (filled > 0) & (covered == 0)
    frac = hole.sum() / max(filled.sum(), 1)
    return hole.astype(np.uint8), float(frac)
