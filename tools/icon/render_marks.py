# Renders the 3D icon marks and the "ctr64" wordmark for the HOME Menu icon and banner.
#   blender -b -P tools/icon/render_marks.py -- <out dir> [design numbers...]
# Writes mark_<n>.png (512x512, transparent) and wordmark.png (transparent) into <out dir>.
# compose.py then puts them on the icon tile and the banner.
import bpy, math, sys, os
from mathutils import Vector

argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
OUT = os.path.abspath(argv[0] if argv else 'tools/icon/out')
ONLY = [int(a) for a in argv[1:]]
os.makedirs(OUT, exist_ok=True)

RED, GREEN, BLUE, YELLOW = (0.89, 0.15, 0.11), (0.10, 0.60, 0.22), (0.08, 0.30, 0.82), (0.98, 0.70, 0.05)
CYAN, GRAPHITE, GREY, LIGHT, WHITE = (0.05, 0.62, 0.92), (0.10, 0.10, 0.12), (0.45, 0.45, 0.50), (0.80, 0.80, 0.83), (0.95, 0.95, 0.95)


def srgb(c):
    return tuple(x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4 for x in c)


_mats = {}


def mat(name, color, rough=0.28, metal=0.0, coat=0.6, emit=0.0):
    key = (name, color, rough, metal, coat, emit)
    if key in _mats:
        return _mats[key]
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    b = m.node_tree.nodes.get('Principled BSDF')
    lin = srgb(color)
    b.inputs['Base Color'].default_value = (*lin, 1)
    b.inputs['Roughness'].default_value = rough
    b.inputs['Metallic'].default_value = metal
    if 'Coat Weight' in b.inputs:
        b.inputs['Coat Weight'].default_value = coat
    if emit:
        b.inputs['Emission Color'].default_value = (*lin, 1)
        b.inputs['Emission Strength'].default_value = emit
    _mats[key] = m
    return m


def link(o, parent, m=None):
    if m is not None:
        o.data.materials.clear()
        if isinstance(m, (list, tuple)):
            for x in m:
                o.data.materials.append(x)
        else:
            o.data.materials.append(m)
    o.parent = parent
    return o


def box(parent, size, loc, m, bevel=0.06, rot=(0, 0, 0)):
    bpy.ops.mesh.primitive_cube_add(size=1, location=loc, rotation=[math.radians(a) for a in rot])
    o = bpy.context.object
    o.scale = size
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    if bevel:
        bv = o.modifiers.new('bevel', 'BEVEL')
        bv.width = bevel
        bv.segments = 4
        bv.limit_method = 'ANGLE'
    bpy.ops.object.shade_smooth()
    return link(o, parent, m)


def cyl(parent, r, depth, loc, m, rot=(90, 0, 0), bevel=0.03, verts=48):
    bpy.ops.mesh.primitive_cylinder_add(radius=r, depth=depth, location=loc, vertices=verts,
                                        rotation=[math.radians(a) for a in rot])
    o = bpy.context.object
    if bevel:
        bv = o.modifiers.new('bevel', 'BEVEL')
        bv.width = bevel
        bv.segments = 3
        bv.limit_method = 'ANGLE'
    bpy.ops.object.shade_smooth()
    return link(o, parent, m)


def sphere(parent, r, loc, m, scale=(1, 1, 1)):
    bpy.ops.mesh.primitive_uv_sphere_add(radius=r, location=loc, segments=48, ring_count=24)
    o = bpy.context.object
    o.scale = scale
    bpy.ops.object.shade_smooth()
    return link(o, parent, m)


def text(parent, body, size, loc, mats, extrude=0.12, bevel=0.025, offset=0.0, facing='-Y', spacing=1.0):
    cu = bpy.data.curves.new('t', 'FONT')
    cu.body = body
    cu.size = size
    cu.extrude = extrude
    cu.bevel_depth = bevel
    cu.bevel_resolution = 3
    cu.offset = offset
    cu.space_character = spacing
    cu.align_x = 'CENTER'
    cu.align_y = 'CENTER'
    o = bpy.data.objects.new('t', cu)
    bpy.context.collection.objects.link(o)
    o.location = loc
    if facing == '-Y':
        o.rotation_euler = (math.radians(90), 0, 0)
    if isinstance(mats, (list, tuple)):
        for m in mats:
            cu.materials.append(m)
    else:
        cu.materials.append(mats)
    o.parent = parent
    return o


def arc(parent, r, a0, a1, thick, m, n=24):
    cu = bpy.data.curves.new('arc', 'CURVE')
    cu.dimensions = '3D'
    cu.bevel_depth = thick
    cu.bevel_resolution = 6
    cu.use_fill_caps = True
    sp = cu.splines.new('POLY')
    sp.points.add(n - 1)
    for i in range(n):
        a = math.radians(a0 + (a1 - a0) * i / (n - 1))
        sp.points[i].co = (r * math.cos(a), 0, r * math.sin(a), 1)
    o = bpy.data.objects.new('arc', cu)
    bpy.context.collection.objects.link(o)
    cu.materials.append(m)
    o.parent = parent
    return o


def new_root(rx, rz):
    bpy.ops.object.empty_add(location=(0, 0, 0))
    root = bpy.context.object
    root.rotation_euler = (math.radians(rx), 0, math.radians(rz))
    return root


# ---- the six designs (built facing the camera: front = -Y, up = +Z) ----

def d1(root):  # four-color blocks
    tiles = [(-0.53, 0.53, RED, 'c', WHITE), (0.53, 0.53, GREEN, 't', WHITE),
             (-0.53, -0.53, BLUE, 'r', WHITE), (0.53, -0.53, YELLOW, '64', WHITE)]
    for x, z, c, ch, tc in tiles:
        box(root, (0.98, 0.98, 0.98), (x, 0, z), mat('t', c), bevel=0.12)
        text(root, ch, 0.62 if len(ch) == 1 else 0.5, (x, -0.49, z + 0.03), mat('l', tc), extrude=0.06,
             bevel=0.012, offset=0.012 if len(ch) == 1 else 0.0)


def d2(root):  # clamshell
    shell = mat('shell', (0.30, 0.30, 0.34), rough=0.35)
    box(root, (2.2, 0.16, 1.35), (0, 0, 0.72), shell, bevel=0.06)
    box(root, (1.75, 0.04, 1.02), (0, -0.08, 0.74), mat('scr', (0.02, 0.02, 0.03), rough=0.1), bevel=0.01)
    text(root, '64', 0.78, (0, -0.12, 0.70), [mat('r', RED, emit=0.4)], extrude=0.08, bevel=0.02, offset=0.015)
    lower = bpy.data.objects.new('lower', None)
    bpy.context.collection.objects.link(lower)
    lower.parent = root
    lower.rotation_euler = (math.radians(-55), 0, 0)
    box(lower, (2.2, 0.18, 1.3), (0, 0, -0.68), mat('shell2', (0.55, 0.55, 0.60), rough=0.35), bevel=0.06)
    box(lower, (1.05, 0.04, 0.82), (0, -0.09, -0.72), mat('touch', CYAN, emit=0.25), bevel=0.01)
    cyl(lower, 0.17, 0.08, (-0.82, -0.12, -0.45), mat('pad', LIGHT))
    for (dx, dz, c) in [(0, 0.15, RED), (0, -0.15, GREEN), (-0.15, 0, BLUE), (0.15, 0, YELLOW)]:
        sphere(lower, 0.075, (0.82 + dx, -0.11, -0.5 + dz), mat('b', c), scale=(1, 0.6, 1))


def d3(root):  # cartridge
    body = mat('cart', (0.55, 0.55, 0.60), rough=0.45)
    box(root, (2.0, 0.42, 2.2), (0, 0, 0), body, bevel=0.1)
    box(root, (1.6, 0.42, 0.25), (0, 0, 1.18), body, bevel=0.08)
    for i in range(5):
        box(root, (1.3, 0.05, 0.05), (0, -0.22, 0.95 - i * 0.1), mat('ridge', (0.40, 0.40, 0.45)), bevel=0.02)
    box(root, (1.55, 0.06, 1.25), (0, -0.22, -0.2), mat('label', CYAN, rough=0.3), bevel=0.03)
    text(root, 'ctr64', 0.46, (0, -0.26, -0.05), mat('w', WHITE), extrude=0.05, bevel=0.008, offset=0.0,
         spacing=1.08)
    for i, c in enumerate([RED, YELLOW, GREEN, BLUE]):
        box(root, (0.3, 0.06, 0.1), (-0.48 + i * 0.32, -0.26, -0.55), mat('s', c), bevel=0.02)


def d4(root):  # chip (for the dark tile)
    pin = mat('pin', (0.80, 0.80, 0.84), rough=0.2, metal=1.0)
    box(root, (1.9, 0.32, 1.9), (0, 0, 0), mat('chip', (0.16, 0.17, 0.19), rough=0.5), bevel=0.06)
    for i in range(5):
        p = -0.72 + i * 0.36
        box(root, (0.16, 0.12, 0.35), (p, 0.02, 1.1), pin, bevel=0.02)
        box(root, (0.16, 0.12, 0.35), (p, 0.02, -1.1), pin, bevel=0.02)
        box(root, (0.35, 0.12, 0.16), (1.1, 0.02, p), pin, bevel=0.02)
        box(root, (0.35, 0.12, 0.16), (-1.1, 0.02, p), pin, bevel=0.02)
    text(root, '64', 0.95, (0, -0.17, 0.0), mat('c', CYAN, emit=1.2), extrude=0.1, bevel=0.02, offset=0.02)
    for (x, z, c) in [(-0.72, 0.72, RED), (0.72, 0.72, YELLOW), (-0.72, -0.72, BLUE), (0.72, -0.72, GREEN)]:
        cyl(root, 0.09, 0.06, (x, -0.17, z), mat('d', c, emit=0.6), bevel=0.01)


SIX = [".###.", "#....", "#....", "####.", "#...#", "#...#", ".###."]
FOUR = ["...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."]


def d5(root):  # voxel 64
    c = 0.3
    for d, (rows, col) in enumerate([(SIX, RED), (FOUR, BLUE)]):
        for r, row in enumerate(rows):
            for k, ch in enumerate(row):
                if ch == '#':
                    x = (-5.5 + d * 6 + k) * c + c / 2
                    box(root, (c * 0.96, c * 1.2, c * 0.96), (x, 0, (3 - r) * c), mat('v', col), bevel=0.03)
    box(root, (1.45, 0.36, 0.16), (-0.9 + 0.075, 0, -1.25), mat('y', YELLOW), bevel=0.04)
    box(root, (1.45, 0.36, 0.16), (0.9 + 0.075 - 0.15 + 0.15, 0, -1.25), mat('g', GREEN), bevel=0.04)


def d6(root):  # stick gate
    for i, c in enumerate([RED, YELLOW, GREEN, BLUE]):
        a0 = 135 - i * 90 + 6
        arc(root, 1.05, a0, a0 - 78, 0.22, mat('a', c))
    cyl(root, 0.5, 0.3, (0, 0.05, 0), mat('base', (0.25, 0.25, 0.28), rough=0.4), bevel=0.05)
    cyl(root, 0.18, 0.5, (0, -0.25, 0), mat('stem', (0.35, 0.35, 0.38)), bevel=0.02)
    sphere(root, 0.42, (0, -0.55, 0), mat('cap', CYAN, rough=0.25), scale=(1, 0.45, 1))


def lowpoly_mat(name, texels, size=32):
    """N64 look: flat facets with per-face colour (the 'col' attribute) times a small blotchy
    texture sampled without filtering"""
    import random
    rnd = random.Random(64)
    img = bpy.data.images.new(name + '_tex', size, size)
    px = []
    for _ in range(size * size):
        k = rnd.uniform(*texels)
        warm = rnd.uniform(-0.04, 0.04)
        px += [min(1, k + warm), k, max(0, k - warm), 1]
    img.pixels = px
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    b = nt.nodes['Principled BSDF']
    b.inputs['Roughness'].default_value = 0.55
    if 'Specular IOR Level' in b.inputs:
        b.inputs['Specular IOR Level'].default_value = 0.25
    att = nt.nodes.new('ShaderNodeAttribute')
    att.attribute_name = 'col'
    tex = nt.nodes.new('ShaderNodeTexImage')
    tex.image = img
    tex.interpolation = 'Closest'
    mul = nt.nodes.new('ShaderNodeMix')
    mul.data_type = 'RGBA'
    mul.blend_type = 'MULTIPLY'
    mul.inputs['Factor'].default_value = 1.0
    nt.links.new(att.outputs['Color'], mul.inputs[6])
    nt.links.new(tex.outputs['Color'], mul.inputs[7])
    nt.links.new(mul.outputs[2], b.inputs['Base Color'])
    return m


def face_colors(o, color_of_face):
    me = o.data
    ca = me.color_attributes.new('col', 'FLOAT_COLOR', 'CORNER')
    for p in me.polygons:
        c = color_of_face(p)
        for li in p.loop_indices:
            ca.data[li].color = (*srgb(c), 1)


def d7(root):  # low-poly orange with a leaf (N64 model style)
    import random
    rnd = random.Random(7)
    bpy.ops.mesh.primitive_uv_sphere_add(segments=14, ring_count=9, radius=1.0)
    o = bpy.context.object
    o.scale = (1.0, 1.0, 0.9)
    bpy.ops.object.transform_apply(scale=True)
    for v in o.data.vertices:  # the dimple where the stem sits
        if v.co.z > 0.85:
            v.co.z -= 0.12
    light = Vector((-0.5, -0.6, 0.65)).normalized()

    def orange(p):
        n = p.normal
        t = max(0.0, n.dot(light))  # painted-in light, as N64 textures often had
        z = n.z
        u = (z + 1) / 2
        base = (0.92 + 0.06 * u, 0.36 + 0.16 * u + 0.1 * t, 0.05)
        j = rnd.uniform(-0.035, 0.035)
        return tuple(min(1, max(0, c + j + 0.06 * t)) for c in base)

    face_colors(o, orange)
    o.data.materials.append(lowpoly_mat('peel', (0.9, 1.0), size=48))
    o.parent = root
    # stem
    bpy.ops.mesh.primitive_cylinder_add(vertices=6, radius=0.09, depth=0.22, location=(0, 0, 0.83))
    s = bpy.context.object
    face_colors(s, lambda p: (0.33, 0.25, 0.08) if p.normal.z < 0.5 else (0.45, 0.40, 0.12))
    s.data.materials.append(lowpoly_mat('stem', (0.85, 1.0)))
    s.parent = root
    # leaf: a low-poly blade folded along its midrib, both halves tilted up a little
    L = 1.3
    prof = [0.0, 0.16, 0.25, 0.28, 0.25, 0.17, 0.0]
    verts, faces = [], []
    for i, w in enumerate(prof):
        x = L * i / (len(prof) - 1)
        lift = 0.16 * math.sin(math.pi * i / (len(prof) - 1))
        verts += [(x, 0, lift), (x, w, lift + w * 0.45), (x, -w, lift + w * 0.45)]
    for i in range(len(prof) - 1):
        a, b = 3 * i, 3 * (i + 1)
        faces += [(a, b, b + 1, a + 1), (a, a + 2, b + 2, b)]
    me = bpy.data.meshes.new('leaf')
    me.from_pydata(verts, [], faces)
    me.update()
    leaf = bpy.data.objects.new('leaf', me)
    bpy.context.collection.objects.link(leaf)
    leaf.location = (0.06, 0, 0.88)
    leaf.rotation_euler = (math.radians(20), math.radians(-14), math.radians(-32))

    def green(p):
        up = p.center.y > 0
        return (0.30, 0.72, 0.18) if up else (0.16, 0.50, 0.12)

    face_colors(leaf, green)
    leaf.data.materials.append(lowpoly_mat('leafm', (0.9, 1.0), size=48))
    leaf.parent = root


def n64_image(name, size, fn):
    img = bpy.data.images.new(name, size, size)
    px = []
    for y in range(size):
        for x in range(size):
            r, g, b = fn(x / (size - 1), y / (size - 1))
            px += [min(1, max(0, r)), min(1, max(0, g)), min(1, max(0, b)), 1]  # byte image: sRGB values
    img.pixels = px
    return img


def n64_mat(name, img, uv_scale=(1, 1)):
    """N64 rendering: no real-time lighting, the texture (small, bilinear-filtered) times the
    per-vertex colour that holds the baked light, interpolated across each triangle"""
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    for nd in list(nt.nodes):
        if nd.type != 'OUTPUT_MATERIAL':
            nt.nodes.remove(nd)
    out = [nd for nd in nt.nodes if nd.type == 'OUTPUT_MATERIAL'][0]
    uv = nt.nodes.new('ShaderNodeUVMap')
    mp = nt.nodes.new('ShaderNodeMapping')
    mp.inputs['Scale'].default_value = (uv_scale[0], uv_scale[1], 1)
    tex = nt.nodes.new('ShaderNodeTexImage')
    tex.image = img
    tex.interpolation = 'Linear'
    att = nt.nodes.new('ShaderNodeAttribute')
    att.attribute_name = 'col'
    mul = nt.nodes.new('ShaderNodeMix')
    mul.data_type = 'RGBA'
    mul.blend_type = 'MULTIPLY'
    mul.inputs['Factor'].default_value = 1.0
    em = nt.nodes.new('ShaderNodeEmission')
    nt.links.new(uv.outputs['UV'], mp.inputs['Vector'])
    nt.links.new(mp.outputs['Vector'], tex.inputs['Vector'])
    nt.links.new(tex.outputs['Color'], mul.inputs[6])
    nt.links.new(att.outputs['Color'], mul.inputs[7])
    nt.links.new(mul.outputs[2], em.inputs['Color'])
    nt.links.new(em.outputs['Emission'], out.inputs['Surface'])
    return m


N64_LIGHT = Vector((-0.55, -0.65, 0.6)).normalized()


def bake_vertex_light(o, albedo_of, ambient=0.42, diffuse=0.78, two_sided=False):
    """Gouraud lighting baked into vertex colours (one directional light plus ambient)"""
    me = o.data
    ca = me.color_attributes.new('col', 'FLOAT_COLOR', 'POINT')
    mw = o.matrix_world.to_3x3()
    for v in me.vertices:
        n = (mw @ v.normal).normalized()
        d = n.dot(N64_LIGHT)
        if two_sided:
            d = abs(d)
        k = ambient + diffuse * max(0.0, d)
        a = albedo_of(v)
        ca.data[v.index].color = (*srgb(tuple(min(1, c * k) for c in a)), 1)


def d7n(root):  # orange as an N64 Zelda item: few polygons, smooth baked light, blurry textures
    import random
    rnd = random.Random(5)
    grid = [[rnd.uniform(-1, 1) for _ in range(9)] for _ in range(9)]

    def noise(u, v):
        x, y = u * 8, v * 8
        i, j = int(x), int(y)
        fx, fy = x - i, y - j
        a = grid[j][i] * (1 - fx) + grid[j][min(i + 1, 8)] * fx
        b = grid[min(j + 1, 8)][i] * (1 - fx) + grid[min(j + 1, 8)][min(i + 1, 8)] * fx
        return a * (1 - fy) + b * fy

    fine = [[rnd.uniform(-1, 1) for _ in range(17)] for _ in range(17)]

    def fnoise(u, v):
        x, y = u * 16, v * 16
        i, j = min(int(x), 15), min(int(y), 15)
        fx, fy = x - i, y - j
        a = fine[j][i] * (1 - fx) + fine[j][i + 1] * fx
        b = fine[j + 1][i] * (1 - fx) + fine[j + 1][i + 1] * fx
        return a * (1 - fy) + b * fy

    def peel(u, v):
        k = 1 + 0.13 * noise(u, v) + 0.07 * fnoise(u, v)
        r = rnd.random()
        if r < 0.10:  # pores, darker
            k -= 0.07
        elif r < 0.14:  # oily specks, lighter
            k += 0.08
        c = (1.0 * k, (0.52 + 0.04 * noise(v, u)) * k, 0.10 * k)
        return tuple(round(min(1, max(0, x)) * 31) / 31 for x in c)  # 5 bits per channel

    bpy.ops.mesh.primitive_uv_sphere_add(segments=10, ring_count=8, radius=1.0)
    o = bpy.context.object
    o.scale = (1.03, 0.99, 0.92)
    bpy.ops.object.transform_apply(scale=True)
    lump = random.Random(11)
    for v in o.data.vertices:
        if v.co.z > 0.8:
            v.co.z -= 0.14
            continue
        a = math.atan2(v.co.y, v.co.x)
        k = 1 + lump.uniform(-0.045, 0.045) + 0.035 * math.sin(2 * a + 0.7) * (1 - abs(v.co.z))
        v.co.x *= k
        v.co.y *= k
        v.co.z *= 1 + lump.uniform(-0.03, 0.03)
    bpy.ops.object.shade_smooth()
    o.parent = root
    bpy.context.view_layer.update()
    o.name = 'orange'
    bake_vertex_light(o, lambda v: (1.0, 0.92 - 0.08 * max(0, -v.co.z), 0.9))
    o.data.materials.append(n64_mat('peel', n64_image('peel_t', 32, peel), uv_scale=(2, 1)))

    def flat_obj(name, verts, faces, uvs=None):
        me = bpy.data.meshes.new(name)
        me.from_pydata(verts, [], faces)
        me.update()
        ob = bpy.data.objects.new(name, me)
        bpy.context.collection.objects.link(ob)
        if uvs:
            uvl = me.uv_layers.new(name='UVMap')
            for p in me.polygons:
                for li in p.loop_indices:
                    uvl.data[li].uv = uvs[me.loops[li].vertex_index]
        return ob

    def plain(r, g, b):
        return n64_image('c%d%d%d' % (r * 99, g * 99, b * 99), 8, lambda u, v: (r, g, b))

    # stem: a five-sided stub, and a five-pointed green crown lying in the dimple
    top = 0.76
    bpy.ops.mesh.primitive_cylinder_add(vertices=5, radius=0.07, depth=0.24, location=(0, 0, top + 0.1))
    st = bpy.context.object
    bpy.ops.object.shade_smooth()
    st.parent = root
    bpy.context.view_layer.update()
    bake_vertex_light(st, lambda v: (0.40, 0.30, 0.12))
    st.data.materials.append(n64_mat('stem', plain(1, 1, 1)))
    cv, cf = [(0, 0, top + 0.02)], []
    for i in range(10):
        a = math.radians(90 + 36 * i)
        r = 0.27 if i % 2 == 0 else 0.1
        cv.append((r * math.cos(a), r * math.sin(a), top - (0.02 if i % 2 == 0 else -0.01)))
    for i in range(10):
        cf.append((0, 1 + i, 1 + (i + 1) % 10))
    cr = flat_obj('crown', cv, cf)
    cr.parent = root
    bpy.context.view_layer.update()
    bake_vertex_light(cr, lambda v: (0.22, 0.55, 0.15), two_sided=True)
    cr.data.materials.append(n64_mat('crown', plain(1, 1, 1)))

    # leaf: a flat diamond folded a little along its midrib, painted texture (gradient, light
    # midrib, faint veins), growing from the crown beside the stem
    def leaf_tex(u, v):
        across = abs(v - 0.5) * 2
        g = (0.30 + 0.25 * (1 - across) - 0.08 * u, 0.62 + 0.20 * (1 - across) - 0.10 * u, 0.14)
        if across < 0.10:
            return (0.62, 0.82, 0.30)
        vein = (u * 7 - across * 3) % 1.0
        if vein < 0.12 and across < 0.85:
            return (g[0] + 0.10, g[1] + 0.10, g[2] + 0.04)
        return g

    # stations along the blade: (position 0..1, half width, midrib lift); a narrow stalk first
    L = 0.95
    st_ = [(0.0, 0.025, 0.0), (0.12, 0.03, 0.02), (0.30, 0.20, 0.05), (0.52, 0.27, 0.06),
           (0.74, 0.21, 0.05), (0.90, 0.10, 0.03), (1.0, 0.0, 0.0)]
    lv, luv, lf = [], [], []
    for t, w, lift in st_:
        x = t * L
        bend = 0.10 * math.sin(math.pi * t)  # the blade arches a little
        lv += [(x, 0, bend + lift), (x, w, bend - w * 0.25), (x, -w, bend - w * 0.25)]
        luv += [(t, 0.5), (t, 1.0), (t, 0.0)]
    for i in range(len(st_) - 1):
        a, b = 3 * i, 3 * (i + 1)
        lf += [(a, b, b + 1, a + 1), (a, a + 2, b + 2, b)]
    leaf = flat_obj('leaf', lv, lf, luv)
    # basis: the blade points up and to the right, slightly toward the viewer; its face turned
    # toward the viewer (normal = local +Z), so it reads as a leaf, not an edge
    from mathutils import Matrix
    d = Vector((1.0, -0.30, 0.55)).normalized()
    nrm = Vector((0.0, -1.0, 0.75))
    nrm = (nrm - d * nrm.dot(d)).normalized()
    side = nrm.cross(d)
    rot = Matrix((d, side, nrm)).transposed().to_4x4()
    leaf.matrix_basis = Matrix.Translation((0.03, 0.07, top + 0.04)) @ rot
    leaf.parent = root
    bpy.context.view_layer.update()
    bake_vertex_light(leaf, lambda v: (1, 1, 1), ambient=0.55, diffuse=0.55, two_sided=True)
    leaf.data.materials.append(n64_mat('leaf', n64_image('leaf_t', 32, leaf_tex)))



FONTS = r'C:\Windows\Fonts'


def n64_text(root, body, font, size, loc, depth, albedo, bevel=0.02, offset=0.0, spacing=1.0,
             rot_x=90.0, side=None, split=True):
    """chunky 3D text as an N64 model: chamfered, edges split, light baked into vertex colours.
    albedo(y) gives the colour at height y in the text's own space (gradients)"""
    cu = bpy.data.curves.new('nt', 'FONT')
    cu.body = body
    cu.font = bpy.data.fonts.load(os.path.join(FONTS, font), check_existing=True)
    cu.size = size
    cu.extrude = depth
    cu.bevel_depth = bevel
    cu.bevel_resolution = 0
    cu.offset = offset
    cu.space_character = spacing
    cu.align_x = 'CENTER'
    cu.align_y = 'CENTER'
    ob = bpy.data.objects.new('nt', cu)
    bpy.context.collection.objects.link(ob)
    ob.location = loc
    ob.rotation_euler = (math.radians(rot_x), 0, 0)
    ob.parent = root
    for x in bpy.context.view_layer.objects:
        x.select_set(False)
    bpy.context.view_layer.objects.active = ob
    ob.select_set(True)
    bpy.ops.object.convert(target='MESH')
    ob = bpy.context.object
    if not split:  # a boolean cutter: keep it closed
        return ob
    es = ob.modifiers.new('es', 'EDGE_SPLIT')
    es.split_angle = math.radians(35)
    bpy.ops.object.modifier_apply(modifier='es')
    bpy.context.view_layer.update()
    ys = [v.co.y for v in ob.data.vertices]
    lo, hi = min(ys), max(ys)

    def alb(v):  # face of the letters: the gradient; the walls: the darker side colour
        front = v.normal.z > 0.6
        t = (v.co.y - lo) / max(hi - lo, 1e-6)
        return albedo(t) if front or side is None else side(t)

    bake_vertex_light(ob, alb)
    ob.data.materials.append(n64_mat('nt', n64_image('white', 4, lambda u, v: (1, 1, 1))))
    return ob


def outlined(root, body, font, size, loc, depth, albedo, edge, edge_w=0.0, **kw):
    """N64 logo lettering: bright gradient faces, darker walls"""
    dark = lambda t: tuple(c * (0.75 + 0.25 * t) for c in edge)
    return n64_text(root, body, font, size, loc, depth, albedo, side=dark, **kw)


def orange_obj():
    return bpy.data.objects['orange']


def carve(root, body, font, size, z=-0.05, depth=0.5, spacing=1.0):
    """letters cut into the peel (a boolean), the cut lit darker"""
    cutter = n64_text(root, body, font, size, (0, -1.0, z), depth, lambda t: (1, 1, 1), bevel=0.0,
                      spacing=spacing, split=False)
    cutter.data.materials.append(bpy.data.materials.new('cut'))
    org = orange_obj()
    sub = org.modifiers.new('sub', 'SUBSURF')  # more vertices for the cut, same low-poly outline
    sub.subdivision_type = 'SIMPLE'
    sub.levels = sub.render_levels = 3
    for x in bpy.context.view_layer.objects:
        x.select_set(False)
    bpy.context.view_layer.objects.active = org
    org.select_set(True)
    bpy.ops.object.modifier_apply(modifier='sub')
    bo = org.modifiers.new('carve', 'BOOLEAN')
    bo.operation = 'DIFFERENCE'
    bo.object = cutter
    bo.solver = 'EXACT'
    bo.use_hole_tolerant = True
    bo.material_mode = 'TRANSFER'
    for x in bpy.context.view_layer.objects:
        x.select_set(False)
    bpy.context.view_layer.objects.active = org
    org.select_set(True)
    bpy.ops.object.modifier_apply(modifier='carve')
    bpy.data.objects.remove(cutter)
    org.data.color_attributes.remove(org.data.color_attributes['col'])
    bpy.context.view_layer.update()

    me = org.data
    ca = me.color_attributes.new('col', 'FLOAT_COLOR', 'CORNER')
    mw = org.matrix_world.to_3x3()
    peel_mat = me.materials[0]
    for poly in me.polygons:
        cut = me.materials[poly.material_index] != peel_mat
        for li in poly.loop_indices:
            v = me.vertices[me.loops[li].vertex_index]
            n = (mw @ (poly.normal if cut else v.normal)).normalized()
            k = 0.42 + 0.78 * max(0.0, n.dot(N64_LIGHT))
            a = (0.62, 0.36, 0.30) if cut else (1.0, 0.92 - 0.08 * max(0, -v.co.z), 0.9)
            ca.data[li].color = (*srgb(tuple(min(1, c * k) for c in a)), 1)
    for poly in me.polygons:  # one material again: the cut keeps the peel texture, darker
        poly.material_index = 0


def grad(top, bot):
    return lambda t: tuple(bot[i] + (top[i] - bot[i]) * t for i in range(3))


YEL_RED = grad((1.0, 0.92, 0.25), (0.95, 0.30, 0.10))
WHITE_T = grad((1.0, 1.0, 1.0), (0.80, 0.86, 0.95))
GREEN_T = grad((0.55, 0.92, 0.30), (0.12, 0.52, 0.12))
BLUE_T = grad((0.45, 0.75, 1.0), (0.10, 0.30, 0.85))


def v_carve64(root):
    d7n(root)
    carve(root, '64', 'ariblk.ttf', 0.95, z=-0.08)


def v_carve_ctr(root):
    d7n(root)
    carve(root, 'ctr64', 'COOPBL.TTF', 0.48, z=-0.05, spacing=1.08)


def v_logo64(root):
    d7n(root)
    outlined(root, '64', 'GILSANUB.TTF', 1.0, (0.45, -1.15, -0.55), 0.22, YEL_RED, (0.75, 0.25, 0.06))


def v_logo_ctr(root):
    d7n(root)
    outlined(root, 'ctr64', 'BAUHS93.TTF', 0.95, (0, -1.2, -0.62), 0.2, WHITE_T, (0.20, 0.55, 0.15),
             spacing=1.02)


def v_sticker(root):
    d7n(root)
    bpy.ops.mesh.primitive_cylinder_add(vertices=14, radius=0.38, depth=0.04, location=(0.0, -1.0, -0.06),
                                        rotation=(math.radians(90), 0, 0))
    st = bpy.context.object
    st.scale = (1.25, 1.0, 1.0)
    st.parent = root
    bpy.context.view_layer.update()
    bake_vertex_light(st, lambda v: (0.97, 0.97, 0.93), ambient=0.6, diffuse=0.45)
    st.data.materials.append(n64_mat('stk', n64_image('white2', 4, lambda u, v: (1, 1, 1))))
    n64_text(root, '64', 'SHOWG.TTF', 0.5, (0.0, -1.03, -0.08), 0.03, BLUE_T, bevel=0.005)


def v_snap(root):
    d7n(root)
    outlined(root, '64', 'SNAP____.TTF', 1.05, (0.4, -1.15, -0.5), 0.22, GREEN_T, (0.10, 0.35, 0.08))


def v_ctr_blue(root):
    d7n(root)
    outlined(root, 'ctr64', 'ariblk.ttf', 0.78, (0, -1.2, -0.6), 0.2, BLUE_T, (0.08, 0.18, 0.55),
             spacing=1.0)



def carve2(root, body, font, size, colors, up=0.0, depth=0.5, spacing=1.0, ambient=0.55, sub=3):
    """letters cut into the peel where the orange faces the viewer; the inside of each letter in
    its own colour (colors: one per character), lit by the same baked light"""
    from mathutils import Matrix
    cu = bpy.data.curves.new('cut', 'FONT')
    cu.body = body
    cu.font = bpy.data.fonts.load(os.path.join(FONTS, font), check_existing=True)
    cu.size = size
    cu.extrude = depth
    cu.space_character = spacing
    cu.align_x = 'CENTER'
    cu.align_y = 'CENTER'
    mats = []
    for i, c in enumerate(colors):
        m = n64_mat('cut%d' % i, n64_image('white_cut', 4, lambda u, v: (1, 1, 1)))
        m['carve_color'] = c
        cu.materials.append(m)
        mats.append(m)
    for i in range(len(body)):
        cu.body_format[i].material_index = min(i, len(colors) - 1)
    cutter = bpy.data.objects.new('cut', cu)
    bpy.context.collection.objects.link(cutter)
    # aim at the viewer: the camera looks along +Y, so the surface point facing it is -Y in the
    # world; in the orange's (root's) space that is the inverse root rotation of -Y
    bpy.context.view_layer.update()
    rinv = root.matrix_world.to_3x3().normalized().inverted()
    toward = (rinv @ Vector((0, -1, 0))).normalized()
    upv = (rinv @ Vector((0, 0, 1))).normalized()
    orient = rinv.to_4x4() @ Matrix.Rotation(math.radians(90), 4, 'X')
    cutter.parent = root
    cutter.matrix_basis = Matrix.Translation(toward * 1.0 + upv * up) @ orient
    for x in bpy.context.view_layer.objects:
        x.select_set(False)
    bpy.context.view_layer.objects.active = cutter
    cutter.select_set(True)
    bpy.ops.object.convert(target='MESH')
    cutter = bpy.context.object
    org = orange_obj()
    levels = sub
    sub = org.modifiers.new('sub', 'SUBSURF')
    sub.subdivision_type = 'SIMPLE'
    sub.levels = sub.render_levels = levels
    for x in bpy.context.view_layer.objects:
        x.select_set(False)
    bpy.context.view_layer.objects.active = org
    org.select_set(True)
    bpy.ops.object.modifier_apply(modifier='sub')
    bo = org.modifiers.new('carve', 'BOOLEAN')
    bo.operation = 'DIFFERENCE'
    bo.object = cutter
    bo.solver = 'EXACT'
    bo.use_hole_tolerant = True
    bo.material_mode = 'TRANSFER'
    bpy.ops.object.modifier_apply(modifier='carve')
    bpy.data.objects.remove(cutter)
    me = org.data
    me.color_attributes.remove(me.color_attributes['col'])
    ca = me.color_attributes.new('col', 'FLOAT_COLOR', 'CORNER')
    mw = org.matrix_world.to_3x3()
    peel_mat = me.materials[0]
    for poly in me.polygons:
        m = me.materials[poly.material_index]
        cut = m != peel_mat
        a = tuple(m['carve_color']) if cut else None
        for li in poly.loop_indices:
            v = me.vertices[me.loops[li].vertex_index]
            n = (mw @ (poly.normal if cut else v.normal)).normalized()
            d = max(0.0, n.dot(N64_LIGHT))
            k = (ambient + 0.6 * d) if cut else (0.42 + 0.78 * d)
            c = a if cut else (1.0, 0.92 - 0.08 * max(0, -v.co.z), 0.9)
            ca.data[li].color = (*srgb(tuple(min(1, x * k) for x in c)), 1)


FLESH = (1.0, 0.80, 0.32)
PITH = (1.0, 0.97, 0.86)
APP_CYAN = (0.13, 0.72, 0.94)
N_RED, N_YEL, N_GRN, N_BLU = (0.92, 0.18, 0.14), (1.0, 0.80, 0.10), (0.18, 0.66, 0.24), (0.15, 0.40, 0.90)


def v_cut_flesh(root):
    d7n(root)
    carve2(root, 'ctr64', 'COOPBL.TTF', 0.5, [FLESH], spacing=1.08)


def v_cut_pith(root):
    d7n(root)
    carve2(root, 'ctr64', 'COOPBL.TTF', 0.5, [PITH], spacing=1.08)


def v_cut_cyan(root):
    d7n(root)
    carve2(root, 'ctr64', 'COOPBL.TTF', 0.5, [APP_CYAN], spacing=1.08)


def v_cut_n64(root):
    d7n(root)
    carve2(root, 'ctr64', 'COOPBL.TTF', 0.5, [N_RED, N_YEL, N_GRN, N_BLU, N_RED], spacing=1.08)


def v_cut_mix(root):
    d7n(root)
    carve2(root, 'ctr64', 'COOPBL.TTF', 0.5, [APP_CYAN, APP_CYAN, APP_CYAN, N_RED, N_BLU], spacing=1.08)



def flat_bake(o, albedo_of_face, ambient=0.5, diffuse=0.65, two_sided=False):
    """flat shading: one lit colour per face (corner domain, the face normal)"""
    me = o.data
    if 'col' in me.color_attributes:
        me.color_attributes.remove(me.color_attributes['col'])
    ca = me.color_attributes.new('col', 'FLOAT_COLOR', 'CORNER')
    mw = o.matrix_world.to_3x3()
    for poly in me.polygons:
        n = (mw @ poly.normal).normalized()
        d = n.dot(N64_LIGHT)
        d = abs(d) if two_sided else max(0.0, d)
        a = albedo_of_face(poly)
        if a is None:
            continue
        k = ambient + diffuse * d
        for li in poly.loop_indices:
            ca.data[li].color = (*srgb(tuple(min(1, c * k) for c in a)), 1)
    for poly in me.polygons:
        if albedo_of_face(poly) is None:
            for li in poly.loop_indices:
                pass


def edge_lines(src, edges, radius, name, push=0.0):
    """black tubes along the given edges (vertex pairs in src's local space)"""
    vs, es, index = [], [], {}
    for a, b in edges:
        for v in (a, b):
            key = (round(v.x, 5), round(v.y, 5), round(v.z, 5))
            if key not in index:
                index[key] = len(vs)
                vs.append(v * (1 + push))
        es.append((index[(round(a.x, 5), round(a.y, 5), round(a.z, 5))],
                   index[(round(b.x, 5), round(b.y, 5), round(b.z, 5))]))
    me = bpy.data.meshes.new(name)
    me.from_pydata(vs, es, [])
    me.update()
    ob = bpy.data.objects.new(name, me)
    bpy.context.collection.objects.link(ob)
    ob.parent = src.parent
    ob.matrix_basis = src.matrix_basis.copy()
    for x in bpy.context.view_layer.objects:
        x.select_set(False)
    bpy.context.view_layer.objects.active = ob
    ob.select_set(True)
    bpy.ops.object.convert(target='CURVE')
    ob = bpy.context.object
    ob.data.bevel_depth = radius
    ob.data.bevel_resolution = 2
    ob.data.use_fill_caps = True
    ob.data.materials.append(n64_mat('ink', n64_image('ink_t', 4, lambda u, v: (0, 0, 0))))
    return ob


def part(mname):
    return next(o for o in bpy.data.objects if o.type == 'MESH' and o.data.materials
                and o.data.materials[0].name.startswith(mname))


def mesh_edges(o):
    me = o.data
    return [(me.vertices[e.vertices[0]].co.copy(), me.vertices[e.vertices[1]].co.copy()) for e in me.edges]


def v_wire(root, inside=None):
    """faceted low-poly orange, every edge inked black, ctr64 carved with a heavy outline"""
    import bmesh, random
    d7n(root)
    org = orange_obj()
    bm = bmesh.new()
    bm.from_mesh(org.data)
    bmesh.ops.triangulate(bm, faces=bm.faces[:], quad_method='BEAUTY')
    bm.to_mesh(org.data)
    bm.free()
    org.data.update()
    peel_edges = mesh_edges(org)
    for nm in ('stem', 'crown', 'leaf'):
        ob = part(nm)
        edge_lines(ob, mesh_edges(ob), 0.0045, nm + '_ink')
    carve2(root, 'ctr64', 'COOPBL.TTF', 0.5, [inside or PITH], spacing=1.08, ambient=0.62)
    org = orange_obj()
    me = org.data
    # the carved letters' rim: edges between the peel and a cut face
    bm = bmesh.new()
    bm.from_mesh(me)
    rim = []
    for e in bm.edges:
        ms = {f.material_index for f in e.link_faces}
        if 0 in ms and len(ms) > 1:
            rim.append((e.verts[0].co.copy(), e.verts[1].co.copy()))
    bm.free()
    edge_lines(org, rim, 0.017, 'rim_ink')
    # the peel's ink lines stop where the letters are cut away (sub-segments whose middle is no
    # longer on the surface)
    from mathutils.bvhtree import BVHTree
    tree = BVHTree.FromPolygons([v.co for v in me.vertices], [tuple(p.vertices) for p in me.polygons])
    keep = []
    for a, b in peel_edges:
        n = 12
        for i in range(n):
            p0, p1 = a.lerp(b, i / n), a.lerp(b, (i + 1) / n)
            hit = tree.find_nearest((p0 + p1) / 2)
            if hit[0] is not None and hit[3] < 0.02 and me.polygons[hit[2]].material_index == 0:
                keep.append((p0, p1))
    edge_lines(org, keep, 0.0055, 'orange_ink', push=0.003)
    # the peel: flat facets, one warm orange per original triangle (a little variation)
    me.materials[0] = n64_mat('peel_flat', n64_image('white_peel', 4, lambda u, v: (1, 1, 1)))
    rnd = {}

    def peel_col(poly):
        if poly.material_index != 0:
            return None
        key = tuple(round(c, 2) for c in poly.normal)
        if key not in rnd:
            r = random.Random(hash(key))
            t = r.uniform(-1, 1)
            rnd[key] = (1.0, 0.56 + 0.05 * t, 0.13 + 0.03 * t)
        return rnd[key]

    # keep the carve2 colours on the cut faces; recolour the peel only
    cut_cols = {li: tuple(me.color_attributes['col'].data[li].color) for poly in me.polygons
                if poly.material_index != 0 for li in poly.loop_indices}
    flat_bake(org, peel_col, ambient=0.55, diffuse=0.55)
    ca = me.color_attributes['col']
    for li, c in cut_cols.items():
        ca.data[li].color = c
    st = part('stem')
    flat_bake(st, lambda p: (0.50, 0.36, 0.16), ambient=0.6, diffuse=0.5)
    cr = part('crown')
    flat_bake(cr, lambda p: (0.25, 0.60, 0.18), two_sided=True)
    lf = part('leaf')
    flat_bake(lf, lambda p: (1, 1, 1), ambient=0.65, diffuse=0.45, two_sided=True)


def v_wire_pith(root):
    v_wire(root, PITH)



def apply_mods(o, mods):
    for x in bpy.context.view_layer.objects:
        x.select_set(False)
    bpy.context.view_layer.objects.active = o
    o.select_set(True)
    for kind, kw in mods:
        m = o.modifiers.new(kind.lower(), kind)
        for k, v in kw.items():
            setattr(m, k, v)
        bpy.ops.object.modifier_apply(modifier=m.name)


def principled(name, color, rough, coat=0.0, sss=0.0, bump=None, vary=None, image=None, glow=0.0):
    """a pre-rendered-CG material: colour (or an image), optional noise variation and bump"""
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    b = nt.nodes.get('Principled BSDF')
    b.inputs['Base Color'].default_value = (*srgb(color), 1)
    b.inputs['Roughness'].default_value = rough
    if 'Coat Weight' in b.inputs:
        b.inputs['Coat Weight'].default_value = coat
        b.inputs['Coat Roughness'].default_value = 0.15
    if sss and 'Subsurface Weight' in b.inputs:
        b.inputs['Subsurface Weight'].default_value = sss
        b.inputs['Subsurface Radius'].default_value = (0.3, 0.15, 0.08)
        b.inputs['Subsurface Scale'].default_value = 0.05
    if glow:
        b.inputs['Emission Color'].default_value = (*srgb(color), 1)
        b.inputs['Emission Strength'].default_value = glow
    if image is not None:
        t = nt.nodes.new('ShaderNodeTexImage')
        t.image = image
        nt.links.new(t.outputs['Color'], b.inputs['Base Color'])
    elif vary:
        n = nt.nodes.new('ShaderNodeTexNoise')
        n.inputs['Scale'].default_value = vary[0]
        n.inputs['Detail'].default_value = 4
        r = nt.nodes.new('ShaderNodeValToRGB')
        r.color_ramp.elements[0].color = (*srgb(vary[1]), 1)
        r.color_ramp.elements[1].color = (*srgb(vary[2]), 1)
        r.color_ramp.elements[0].position = 0.35
        r.color_ramp.elements[1].position = 0.65
        nt.links.new(n.outputs['Fac'], r.inputs['Fac'])
        nt.links.new(r.outputs['Color'], b.inputs['Base Color'])
    if bump:
        scale, strength = bump
        v = nt.nodes.new('ShaderNodeTexVoronoi')
        v.inputs['Scale'].default_value = scale
        bp = nt.nodes.new('ShaderNodeBump')
        bp.inputs['Strength'].default_value = strength
        bp.inputs['Distance'].default_value = 0.02
        bp.invert = True
        nt.links.new(v.outputs['Distance'], bp.inputs['Height'])
        nt.links.new(bp.outputs['Normal'], b.inputs['Normal'])
    return m


def leaf_image(size=256):
    def f(u, v):
        across = abs(v - 0.5) * 2
        g = (0.20 + 0.20 * (1 - across) - 0.06 * u, 0.55 + 0.22 * (1 - across) - 0.10 * u, 0.10)
        if across < 0.06:
            return (0.66, 0.86, 0.36)
        vein = (u * 7 - across * 3) % 1.0
        if vein < 0.06 and across < 0.88:
            return (g[0] + 0.14, g[1] + 0.12, g[2] + 0.06)
        return g
    return n64_image('leaf_hq', size, f)


def v_menu(root, floor=(1.0, 0.95, 0.82), floor_glow=0.2, wall_col=None):
    """the same orange as a pre-rendered menu item: smoothed model, real materials, ray traced"""
    d7n(root)
    s = bpy.context.scene
    s.render.engine = 'CYCLES'
    s.cycles.samples = 96
    s.cycles.use_denoising = True
    s.view_settings.exposure = -0.55
    org = orange_obj()
    apply_mods(org, [('SUBSURF', {'levels': 2, 'render_levels': 2})])
    carve2(root, 'ctr64', 'COOPBL.TTF', 0.5, [PITH], spacing=1.08)
    org = orange_obj()
    me = org.data
    peel = principled('peel_hq', (1.0, 0.40, 0.0), 0.34, coat=0.3, vary=(4, (1.0, 0.36, 0.0), (1.0, 0.46, 0.02)),
                      bump=(90, 0.3))
    pith = principled('pith_hq', floor, 0.55, sss=0.3, glow=floor_glow)
    wall = principled('wall_hq', wall_col, 0.6) if wall_col else pith  # default: the whole cut in pith
    rinv = root.matrix_world.to_3x3().normalized().inverted()
    toward = (rinv @ Vector((0, -1, 0))).normalized()
    was = [poly.material_index for poly in me.polygons]
    me.materials.clear()
    for m in (peel, pith, wall):
        me.materials.append(m)
    for poly, w in zip(me.polygons, was):  # cut faces: floor (facing out) pith, walls dark
        poly.material_index = 0 if w == 0 else (1 if poly.normal.dot(toward) > 0.7 else 2)
        poly.use_smooth = w == 0
    old = part('stem')  # a rounder stem: twelve sides, slightly tapered, flat cut top
    loc, par = old.matrix_basis.copy(), old.parent
    bpy.data.objects.remove(old)
    bpy.ops.mesh.primitive_cone_add(vertices=12, radius1=0.075, radius2=0.06, depth=0.26)
    st = bpy.context.object
    st.parent = par
    st.matrix_basis = loc
    bpy.ops.object.shade_smooth()
    for poly in st.data.polygons:
        poly.use_smooth = abs(poly.normal.z) < 0.9
    st.data.materials.append(None)
    st.data.materials[0] = principled('stem_hq', (0.36, 0.22, 0.09), 0.75, vary=(30, (0.28, 0.16, 0.06),
                                                                                (0.45, 0.30, 0.13)))
    cr = part('crown')
    apply_mods(cr, [('SOLIDIFY', {'thickness': 0.025}), ('SUBSURF', {'levels': 1, 'render_levels': 1})])
    cr.data.materials[0] = principled('crown_hq', (0.20, 0.50, 0.12), 0.45)
    lf = part('leaf')
    apply_mods(lf, [('SOLIDIFY', {'thickness': 0.02}), ('SUBSURF', {'levels': 2, 'render_levels': 2})])
    bpy.ops.object.shade_smooth()
    lf.data.materials[0] = principled('leaf_hq', (0.3, 0.6, 0.15), 0.3, coat=0.5, image=leaf_image())


def v_menu_dark(root):
    v_menu(root, floor=(0.50, 0.12, 0.03), floor_glow=0.15, wall_col=(0.20, 0.04, 0.01))


def v_menu_cream(root):
    v_menu(root, floor=(1.0, 0.95, 0.82), floor_glow=0.9, wall_col=(0.25, 0.06, 0.01))


DESIGNS = {7: (d7n, 20, -20), 41: (v_menu_dark, 14, -14), 42: (v_menu_cream, 14, -14), 40: (v_menu, 14, -14), 36: (v_wire_pith, 14, -14), 31: (v_cut_flesh, 14, -14), 32: (v_cut_pith, 14, -14), 33: (v_cut_cyan, 14, -14), 34: (v_cut_n64, 14, -14), 35: (v_cut_mix, 14, -14), 21: (v_carve64, 14, -16), 22: (v_carve_ctr, 14, -14), 23: (v_logo64, 16, -18), 24: (v_logo_ctr, 14, -14), 25: (v_sticker, 16, -18), 26: (v_snap, 16, -18), 27: (v_ctr_blue, 14, -14), 1: (d1, 14, -28), 2: (d2, 10, -24), 3: (d3, 10, -26), 4: (d4, 12, -24), 5: (d5, 12, -26), 6: (d6, 16, -20)}


# ---- scene ----

def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    _mats.clear()
    s = bpy.context.scene
    for eng in ('BLENDER_EEVEE', 'BLENDER_EEVEE_NEXT', 'CYCLES'):
        try:
            s.render.engine = eng
            break
        except TypeError:
            pass
    if s.render.engine == 'CYCLES':
        s.cycles.samples = 64
    else:
        try:
            s.eevee.taa_render_samples = 64
        except AttributeError:
            pass
    s.render.film_transparent = True
    s.view_settings.view_transform = 'Standard'
    s.render.image_settings.file_format = 'PNG'
    s.render.image_settings.color_mode = 'RGBA'
    w = bpy.data.worlds.new('w')
    w.use_nodes = True
    w.node_tree.nodes['Background'].inputs[0].default_value = (0.55, 0.57, 0.62, 1)
    w.node_tree.nodes['Background'].inputs[1].default_value = 0.6
    s.world = w
    for name, loc, power, size in [('key', (-4, -6, 6), 1400, 5), ('fill', (5, -5, 1), 500, 4), ('rim', (1, 5, 5), 900, 4),
                                   ('top', (0, -1, 7), 400, 6)]:
        ld = bpy.data.lights.new(name, 'AREA')
        ld.energy = power
        ld.size = size
        lo = bpy.data.objects.new(name, ld)
        s.collection.objects.link(lo)
        lo.location = loc
        lo.rotation_euler = (Vector((0, 0, 0)) - Vector(loc)).to_track_quat('-Z', 'Y').to_euler()
    cd = bpy.data.cameras.new('cam')
    cd.lens = 70
    cam = bpy.data.objects.new('cam', cd)
    s.collection.objects.link(cam)
    cam.location = (0, -12, 0)
    cam.rotation_euler = (math.radians(90), 0, 0)
    s.camera = cam
    return s, cam


def fit(root, s, cam, fill):
    """scale and centre root so its projected size fills `fill` of the frame"""
    bpy.context.view_layer.update()
    dg = bpy.context.evaluated_depsgraph_get()
    pts = []
    for o in bpy.data.objects:
        if o.type in ('MESH', 'FONT', 'CURVE'):
            e = o.evaluated_get(dg)
            me = e.to_mesh()
            pts += [e.matrix_world @ v.co for v in me.vertices]
            e.to_mesh_clear()
    lo = Vector((min(p.x for p in pts), min(p.y for p in pts), min(p.z for p in pts)))
    hi = Vector((max(p.x for p in pts), max(p.y for p in pts), max(p.z for p in pts)))
    rx, rz = s.render.resolution_x, s.render.resolution_y
    view_w = 2 * 12 * 18 / cam.data.lens * (1 if rx >= rz else rx / rz)
    view_h = view_w * rz / rx
    k = min(fill * view_w / (hi.x - lo.x), fill * view_h / (hi.z - lo.z))
    root.scale = (k, k, k)
    c = (lo + hi) / 2
    root.location = (-c.x * k, -c.y * k, -c.z * k)


def refit(root, s, cam, fill):
    """correct the framing from a small test render: the visible pixels' box is centred and
    scaled to `fill` of the frame (geometry bounds miss perspective)"""
    rx, rz = s.render.resolution_x, s.render.resolution_y
    pct = s.render.resolution_percentage
    s.render.resolution_percentage = 25
    tmp = os.path.join(OUT, '_fit.png')
    s.render.filepath = tmp
    bpy.ops.render.render(write_still=True)
    s.render.resolution_percentage = pct
    img = bpy.data.images.load(tmp)
    w, h = img.size
    a = img.pixels[:]
    xs, ys = [], []
    for y in range(h):
        row = y * w * 4
        for x in range(w):
            if a[row + x * 4 + 3] > 0.05:
                xs.append(x)
                ys.append(y)
    bpy.data.images.remove(img)
    os.remove(tmp)
    if not xs:
        return
    bw, bh = max(xs) - min(xs) + 1, max(ys) - min(ys) + 1
    cx, cy = (max(xs) + min(xs)) / 2 - w / 2, (max(ys) + min(ys)) / 2 - h / 2
    view_w = 2 * 12 * 18 / cam.data.lens * (1 if rx >= rz else rx / rz)
    unit = view_w / w  # world units per test pixel at the object's distance
    k = min(fill * w / bw, fill * h / bh)
    root.location.x -= cx * unit
    root.location.z -= cy * unit
    root.location = root.location * k
    root.scale = root.scale * k


def render(path):
    bpy.context.scene.render.filepath = path
    bpy.ops.render.render(write_still=True)

def id_maps(n, s):
    """shade_N.png (the render, 1024) and idmap_N.png: each part one flat colour, letter floor and
    letter walls apart, for tools/icon/trace_svg.py"""
    s.render.resolution_x = s.render.resolution_y = 1024
    render(os.path.join(OUT, 'shade_%d.png' % n))
    for img in bpy.data.images:  # light only: every texture flattened to its average colour
        if img.size[0] and img.name.endswith('_t'):
            px = list(img.pixels)
            m = [sum(px[c::4]) / (len(px) // 4) for c in range(4)]
            img.pixels = m * (len(px) // 4)
    render(os.path.join(OUT, 'light_%d.png' % n))

    def flat(name, c):
        m = bpy.data.materials.new(name)
        m.use_nodes = True
        nt = m.node_tree
        nt.nodes.clear()
        e = nt.nodes.new('ShaderNodeEmission')
        e.inputs[0].default_value = (*c, 1)
        o = nt.nodes.new('ShaderNodeOutputMaterial')
        nt.links.new(e.outputs[0], o.inputs[0])
        return m

    ids = {'peel': (1, 0, 0), 'floor': (1, 1, 1), 'wall': (0, 1, 1), 'leaf': (0, 1, 0), 'stem': (0, 0, 1),
           'crown': (1, 1, 0)}
    fm = {k: flat('id_' + k, c) for k, c in ids.items()}
    org = bpy.data.objects.get('orange')
    for o in bpy.data.objects:
        if o.type != 'MESH' or not o.data.materials:
            continue
        name = o.data.materials[0].name
        if o == org:
            me = o.data
            root = o.parent
            rinv = root.matrix_world.to_3x3().normalized().inverted()
            toward = (rinv @ Vector((0, -1, 0))).normalized()
            was = [p.material_index for p in me.polygons]
            me.materials.clear()
            for k in ('peel', 'floor', 'wall'):
                me.materials.append(fm[k])
            for poly, w in zip(me.polygons, was):
                poly.material_index = 0 if w == 0 else (1 if poly.normal.dot(toward) > 0.7 else 2)
            continue
        for k in ('leaf', 'stem', 'crown'):
            if name.startswith(k):
                o.data.materials.clear()
                o.data.materials.append(fm[k])
    s.render.filter_size = 0.0
    render(os.path.join(OUT, 'idmap_%d.png' % n))




for n, (fn, rx, rz) in DESIGNS.items():
    if ONLY and n not in ONLY:
        continue
    s, cam = reset()
    s.render.resolution_x = s.render.resolution_y = 512
    root = new_root(rx, rz)
    fn(root)
    fit(root, s, cam, 0.86)
    refit(root, s, cam, 0.88)
    refit(root, s, cam, 0.88)
    render(os.path.join(OUT, 'mark_%d.png' % n))
    if os.environ.get('CTR64_IDMAP'):  # for tracing to vector: a large render and a flat ID map
        id_maps(n, s)

if not ONLY or 8 in ONLY:  # the wordmark in the low-poly style: chamfered, matte, flat-lit
    s, cam = reset()
    s.render.resolution_x, s.render.resolution_y = 1024, 384
    root = new_root(6, -8)
    ink = [mat('o', (0.96, 0.52, 0.06), rough=0.6, coat=0.0), mat('g', (0.25, 0.62, 0.16), rough=0.6, coat=0.0)]
    t = text(root, 'ctr64', 1.0, (0, 0, 0), ink, extrude=0.16, bevel=0.035, offset=0.0, spacing=1.12)
    t.data.bevel_resolution = 0
    for i in (3, 4):
        t.data.body_format[i].material_index = 1
    fit(root, s, cam, 0.84)
    render(os.path.join(OUT, 'wordmark_n64.png'))

if not ONLY or 0 in ONLY:
    s, cam = reset()
    s.render.resolution_x, s.render.resolution_y = 1024, 384
    root = new_root(6, -8)
    t = text(root, 'ctr64', 1.0, (0, 0, 0), [mat('ink', (0.22, 0.22, 0.25)), mat('r', RED), mat('b', BLUE)],
             extrude=0.14, bevel=0.02, offset=0.012, spacing=1.12)
    for i in (3, 4):
        t.data.body_format[i].material_index = 1 if i == 3 else 2
    fit(root, s, cam, 0.84)
    render(os.path.join(OUT, 'wordmark.png'))
print('RENDER DONE')
