#!/usr/bin/env python3
"""Ottimizzazione di un pose-graph 2D (SE(2)) — per togliere la deriva dell'odometria.

I keyframe sono nodi con posa (x, y, theta). Ogni registrazione tra due keyframe e' un arco con una
misura di posa relativa. Quando la traccia chiude un anello (si ripassa su una zona gia' mappata)
gli archi diventano sovra-vincolanti: l'ottimizzazione (Gauss-Newton) ridistribuisce l'errore e la
mappa si raddrizza. Nodo 0 fissato (gauge). Solver denso: ok fino a qualche centinaio di nodi.

Convenzione arco (i, j, (dx, dy, dth), w): posa di j nel sistema di i (dalla registrazione j->i).
"""

import numpy as np


def _wrap(a):
    return (a + np.pi) % (2 * np.pi) - np.pi


def pose_to_xyt(M):
    """2x3 affine (rigido) -> (x, y, theta)."""
    return float(M[0, 2]), float(M[1, 2]), float(np.arctan2(M[1, 0], M[0, 0]))


def xyt_to_pose(x, y, t):
    c, s = np.cos(t), np.sin(t)
    return np.array([[c, -s, x], [s, c, y]], np.float32)


def optimize(poses, edges, iters=12, fix=0):
    """poses: (N,3) [x,y,theta]; edges: lista (i, j, (dx,dy,dth), w). -> poses ottimizzate."""
    P = np.asarray(poses, np.float64).copy()
    N = len(P)
    if N < 3 or not edges:
        return P
    for _ in range(iters):
        H = np.zeros((3 * N, 3 * N))
        b = np.zeros(3 * N)
        for (i, j, z, w) in edges:
            xi, yi, ti = P[i]
            xj, yj, tj = P[j]
            c, s = np.cos(ti), np.sin(ti)
            dxw, dyw = xj - xi, yj - yi
            # posa relativa predetta (j nel frame di i)
            pdx = c * dxw + s * dyw
            pdy = -s * dxw + c * dyw
            pdt = _wrap(tj - ti)
            e = np.array([pdx - z[0], pdy - z[1], _wrap(pdt - z[2])])
            Ji = np.array([[-c, -s, -s * dxw + c * dyw],
                           [s, -c, -c * dxw - s * dyw],
                           [0.0, 0.0, -1.0]])
            Jj = np.array([[c, s, 0.0],
                           [-s, c, 0.0],
                           [0.0, 0.0, 1.0]])
            for (A, a) in ((Ji, i), (Jj, j)):
                b[3 * a:3 * a + 3] += w * (A.T @ e)
                for (B, bb) in ((Ji, i), (Jj, j)):
                    H[3 * a:3 * a + 3, 3 * bb:3 * bb + 3] += w * (A.T @ B)
        # fissa il nodo gauge
        H[3 * fix:3 * fix + 3, :] = 0
        H[:, 3 * fix:3 * fix + 3] = 0
        H[3 * fix:3 * fix + 3, 3 * fix:3 * fix + 3] = np.eye(3)
        b[3 * fix:3 * fix + 3] = 0
        H += 1e-6 * np.eye(3 * N)
        try:
            d = np.linalg.solve(H, -b)
        except np.linalg.LinAlgError:
            break
        P += d.reshape(N, 3)
        P[:, 2] = _wrap(P[:, 2])
        if np.abs(d).max() < 1e-4:
            break
    return P
