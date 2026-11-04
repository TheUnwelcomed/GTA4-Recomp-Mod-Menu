"""A small glTF 2.0 reader (v1.7.44): the scene's meshes with their world
transforms applied, as numpy arrays, for building a vehicle out of a
Sketchfab-style model (scene.gltf + scene.bin + textures/). Only what a car
needs: positions, normals, texture coordinates, triangle indices, the
material of each primitive, the node each mesh hangs on.

    g = Gltf('scene.gltf')
    for prim in g.primitives():  # dicts: node, path, material, pos, nrm, uv, tris
        ...
"""
import json
import os
import struct

import numpy as np

COMPONENT = {5120: ('b', 1), 5121: ('B', 1), 5122: ('h', 2), 5123: ('H', 2), 5125: ('I', 4), 5126: ('f', 4)}
COUNT = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}


def quat_matrix(q):
    x, y, z, w = q
    return np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]], np.float64)


def node_matrix(node):
    """The node's local transform as a 4x4 (column vectors)."""
    if 'matrix' in node:
        return np.array(node['matrix'], np.float64).reshape(4, 4).T
    m = np.eye(4)
    s = np.array(node.get('scale', [1, 1, 1]), np.float64)
    r = quat_matrix(node.get('rotation', [0, 0, 0, 1]))
    t = np.array(node.get('translation', [0, 0, 0]), np.float64)
    m[:3, :3] = r * s[None, :]
    m[:3, 3] = t
    return m


class Gltf:
    def __init__(self, path):
        self.dir = os.path.dirname(os.path.abspath(path))
        self.g = json.load(open(path))
        self.buffers = []
        for b in self.g.get('buffers', []):
            uri = b['uri']
            assert not uri.startswith('data:'), 'embedded buffers are not read'
            self.buffers.append(open(os.path.join(self.dir, uri), 'rb').read())
        self.world = {}
        self.path = {}
        scene = self.g['scenes'][self.g.get('scene', 0)]
        for n in scene['nodes']:
            self._walk(n, np.eye(4), [])

    def _walk(self, n, parent, path):
        node = self.g['nodes'][n]
        m = parent @ node_matrix(node)
        self.world[n] = m
        self.path[n] = path + [node.get('name', 'node%d' % n)]
        for c in node.get('children', []):
            self._walk(c, m, self.path[n])

    def accessor(self, index):
        a = self.g['accessors'][index]
        bv = self.g['bufferViews'][a['bufferView']]
        buf = self.buffers[bv['buffer']]
        fmt, size = COMPONENT[a['componentType']]
        n = COUNT[a['type']]
        count = a['count']
        start = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
        stride = bv.get('byteStride', size * n)
        if stride == size * n:
            data = np.frombuffer(buf, '<' + fmt, count * n, start).reshape(count, n)
        else:
            rows = [struct.unpack_from('<%d%s' % (n, fmt), buf, start + i * stride) for i in range(count)]
            data = np.array(rows)
        if a.get('normalized'):
            data = data.astype(np.float64) / np.iinfo(np.dtype(fmt)).max
        return data

    def material(self, index):
        return self.g['materials'][index] if index is not None else {}

    def image_path(self, texture_index):
        """The file of a texture (by its index in `textures`), or None."""
        if texture_index is None:
            return None
        t = self.g['textures'][texture_index]
        img = self.g['images'][t['source']]
        return os.path.join(self.dir, img['uri']) if 'uri' in img else None

    def primitives(self):
        """Every primitive of every mesh node, in world space."""
        out = []
        for n, node in enumerate(self.g['nodes']):
            if 'mesh' not in node or n not in self.world:
                continue
            m = self.world[n]
            rot = m[:3, :3]
            nrm_m = np.linalg.inv(rot).T  # (normals by the inverse transpose)
            mesh = self.g['meshes'][node['mesh']]
            for prim in mesh['primitives']:
                if prim.get('mode', 4) != 4:
                    continue
                at = prim['attributes']
                pos = self.accessor(at['POSITION']).astype(np.float64)
                pos = pos @ rot.T + m[:3, 3]
                nrm = None
                if 'NORMAL' in at:
                    nrm = self.accessor(at['NORMAL']).astype(np.float64) @ nrm_m.T
                    nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-9)
                uv = self.accessor(at['TEXCOORD_0']).astype(np.float32) if 'TEXCOORD_0' in at else None
                if 'indices' in prim:
                    tris = self.accessor(prim['indices']).reshape(-1).astype(np.int64)
                else:
                    tris = np.arange(len(pos), dtype=np.int64)
                tris = tris[:len(tris) // 3 * 3].reshape(-1, 3)
                if np.linalg.det(rot) < 0:  # (a mirrored node turns its faces inside out)
                    tris = tris[:, ::-1]
                out.append(dict(node=n, path=self.path[n], material=prim.get('material'), pos=pos, nrm=nrm, uv=uv,
                                tris=tris))
        return out
