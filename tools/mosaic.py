#!/usr/bin/env python3
"""Ricostruzione a mosaico dell'impronta da scansioni che si sovrappongono (Goodix 5125).

Idea: mappando in movimento, scansioni adiacenti hanno uno spostamento piccolo ma reale. Si stima
la roto-traslazione tra coppie (coverage.estimate_offset), si costruisce un grafo di pose, si
posizionano le scansioni di ogni componente connessa in un sistema comune (albero di copertura dal
nodo di grado massimo) e si fondono le immagini in un composito. La maschera di copertura rivela i
BUCHI: aree dentro l'inviluppo della mappa non coperte da nessuna scansione -> guidano la mappatura.

Lavora su una lista di (feats, thumb) dove feats=(pts,desc) e thumb e' l'immagine preprocessata
(H,W) uint8 di quella scansione. Niente scala (stesso sensore): roto-traslazione pura."""

import numpy as np
import cv2

import coverage

MIN_INLIERS = 7       # inlier RANSAC minimi per fidarsi di un collegamento tra due scansioni
H, W = 64, 80


def build_graph(items):
    """items: lista di (feats, thumb). -> dict edge (i,j)->(M 2x3 che porta j su i, inlier)."""
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
        adj[i].append((j, _invert(M), inl))   # per andare da i a j serve M^-1? vedi sotto
        adj[j].append((i, M, inl))
    return adj


def _invert(M):
    R = M[:, :2]
    t = M[:, 2]
    Ri = np.linalg.inv(R)
    return np.hstack([Ri, (-Ri @ t).reshape(2, 1)]).astype(np.float32)


def _compose(A, B):
    """A,B 2x3 affini; ritorna A∘B (applica B poi A)."""
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
    """Posiziona le scansioni di una componente in coordinate comuni (albero dal nodo piu' connesso).
    -> dict node -> M (2x3) che porta le coordinate del nodo nel sistema comune."""
    # adiacenza con trasformazione node->vicino
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
        for (q, M_qp) in adj[p]:        # M_qp: p->q ... costruiamo q->root
            if q in T:
                continue
            # vogliamo q->root. Abbiamo p->root (T[p]) e una M che lega p e q.
            # adj memorizza (vicino, trasf p->vicino). Serve vicino->root = T[p] ∘ (p->vicino)^-1? no:
            # se M_qp porta p su q (p->q), allora q->p = inv(M_qp), e q->root = T[p] ∘ (q->p).
            T[q] = _compose(T[p], _invert(M_qp))
            st.append(q)
    return T, root


def render(items, comp, T, margin=40):
    """Fonde le thumbnail della componente nel composito. -> (mosaic u8, coverage count, bbox)."""
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
    """Buchi = aree dentro l'inviluppo della copertura ma non coperte. -> (mask_holes, frazione)."""
    covered = (cnt > 0).astype(np.uint8)
    # inviluppo: chiusura morfologica + fill dei contorni esterni
    k = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (15, 15))
    envelope = cv2.morphologyEx(covered, cv2.MORPH_CLOSE, k)
    cnts, _ = cv2.findContours(envelope, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
    filled = np.zeros_like(envelope)
    cv2.drawContours(filled, cnts, -1, 1, -1)
    hole = (filled > 0) & (covered == 0)
    frac = hole.sum() / max(filled.sum(), 1)
    return hole.astype(np.uint8), float(frac)
