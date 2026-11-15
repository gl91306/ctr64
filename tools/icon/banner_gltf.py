# The HOME Menu 3D banner model: the low-poly N64 orange (render_marks.py design 32) as a glTF
# for pycgfx (glTF -> CGFX), lit live by the HOME Menu instead of baked light, swaying slowly.
#   blender -b --factory-startup -P tools/icon/banner_gltf.py -- <out.glb>
# then: python pycgfx/main.py out.glb banner.cgfx; bannertool makebanner -ci banner.cgfx -a silence.wav ...
# Banner camera (pycgfx banner-camera.gltf): glTF (0, 1, 44.8) looking down -Z, yfov 30 deg.
import bpy, math, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
out = sys.argv[sys.argv.index('--') + 1]
# load the design code without rendering anything (design 999 does not exist)
sys.argv = [sys.argv[0], '--', os.path.join(HERE, 'out'), '999']
src = open(os.path.join(HERE, 'render_marks.py')).read()
g = {'__name__': 'render_marks', '__file__': os.path.join(HERE, 'render_marks.py')}
exec(compile(src, 'render_marks.py', 'exec'), g)
from mathutils import Vector

g['reset']()
root = g['new_root'](14, -14)
g['d7n'](root)
g['carve2'](root, 'ctr64', 'COOPBL.TTF', 0.5, [g['PITH']], spacing=1.08, sub=2)


def pbr(name, color=(1, 1, 1), image=None, rough=0.7, two_sided=False):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    b = nt.nodes.get('Principled BSDF')
    b.inputs['Base Color'].default_value = (*g['srgb'](color), 1)
    b.inputs['Roughness'].default_value = rough
    if image is not None:
        t = nt.nodes.new('ShaderNodeTexImage')
        t.image = image
        t.interpolation = 'Linear'
        nt.links.new(t.outputs['Color'], b.inputs['Base Color'])
    m.use_backface_culling = not two_sided
    return m


img = bpy.data.images
mats = {
    'peel': pbr('peel', image=img['peel_t'], rough=0.45),
    'cut': pbr('pith', g['PITH'], rough=0.9),
    'stem': pbr('stem', (0.42, 0.28, 0.12), rough=0.9),
    'crown': pbr('crown', (0.24, 0.58, 0.16), rough=0.7, two_sided=True),
    'leaf': pbr('leaf', image=img['leaf_t'], rough=0.5, two_sided=True),
}
for o in list(bpy.data.objects):
    if o.type != 'MESH':
        continue
    me = o.data
    for a in [a.name for a in me.color_attributes]:  # light comes from the HOME Menu now
        me.color_attributes.remove(me.color_attributes[a])
    for i, m in enumerate(me.materials):
        key = next(k for k in mats if m.name.startswith(k))
        me.materials[i] = mats[key]
    if o.name == 'orange':  # the peel texture repeats twice around (n64_mat's uv_scale)
        uv = me.uv_layers.active
        for d in uv.data:
            d.uv = (d.uv.x * 2, d.uv.y)

# size and place: ~17 units tall, centred on the banner camera's axis (glTF y = 1 = Blender z)
bpy.context.view_layer.update()
dg = bpy.context.evaluated_depsgraph_get()
pts = []
for o in bpy.data.objects:
    if o.type == 'MESH':
        e = o.evaluated_get(dg)
        pts += [e.matrix_world @ v.co for v in e.data.vertices]
lo = Vector([min(p[i] for p in pts) for i in range(3)])
hi = Vector([max(p[i] for p in pts) for i in range(3)])
k = 17.0 / (hi.z - lo.z)
c = (lo + hi) / 2
bpy.ops.object.empty_add(location=(0, 0, 1.0))
sway = bpy.context.object
sway.name = 'sway'
root.parent = sway
root.location = -c * k
root.scale = (k, k, k)

# a slow sway about the vertical axis: rigid node animation (skinning freezes real HOME Menus)
s = bpy.context.scene
s.render.fps = 30
s.frame_start, s.frame_end = 0, 240
for f, a in ((0, -22), (120, 22), (240, -22)):
    sway.rotation_euler = (0, 0, math.radians(a))
    sway.keyframe_insert('rotation_euler', index=2, frame=f)

for o in list(bpy.data.objects):
    if o.type in ('CAMERA', 'LIGHT'):
        bpy.data.objects.remove(o)
bpy.ops.export_scene.gltf(filepath=out, export_format='GLB', export_animations=True, export_apply=True,
                          export_yup=True, export_cameras=False, export_lights=False)
print('GLTF DONE', len(pts), 'verts, scale', round(k, 2))
