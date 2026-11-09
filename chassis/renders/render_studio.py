"""Rebuild the Keel studio scene from the assembly without changing its source.

Run with Blender in background mode, passing -- --preview for a small proof.
The sensor's external envelope is retained; grille details are visual estimates.
"""
from pathlib import Path
import math
import sys
import zipfile
import xml.etree.ElementTree as ET

import bpy
from mathutils import Vector

OUT = Path(__file__).resolve().parent
SOURCE = OUT.parent / 'Keel_R3_robot_assembly.3mf'
PREVIEW = '--preview' in sys.argv
MM = .001
bpy.ops.wm.read_factory_settings(use_empty=True)
scene = bpy.context.scene
scene.unit_settings.system = 'METRIC'
scene.unit_settings.length_unit = 'MILLIMETERS'

def collection(name):
    c = bpy.data.collections.new(name)
    scene.collection.children.link(c)
    return c

robot = collection('Keel — assembly geometry')
details = collection('HC-SR04 and optical details')
studio = collection('Studio — camera and softboxes')

def rgb(hex_value):
    v = [int(hex_value.lstrip('#')[i:i+2],16)/255 for i in (0,2,4)]
    return tuple(c/12.92 if c<=.04045 else ((c+.055)/1.055)**2.4 for c in v)

def material(name, color, rough=.4, metal=0, texture=False, bevel=True):
    m = bpy.data.materials.new(name)
    m.diffuse_color = (*rgb(color),1)
    m.use_nodes = True
    n = m.node_tree.nodes
    p = n.get('Principled BSDF')
    p.inputs['Base Color'].default_value = (*rgb(color),1)
    p.inputs['Roughness'].default_value = rough
    p.inputs['Metallic'].default_value = metal
    if bevel:
        b = n.new('ShaderNodeBevel')
        b.inputs['Radius'].default_value = .000055
        b.samples = 4
        m.node_tree.links.new(b.outputs['Normal'],p.inputs['Normal'])
    if texture:
        tex = n.new('ShaderNodeTexNoise')
        tex.inputs['Scale'].default_value = 360
        tex.inputs['Detail'].default_value = 2
        bump = n.new('ShaderNodeBump')
        bump.inputs['Strength'].default_value = .13
        bump.inputs['Distance'].default_value = .000025
        m.node_tree.links.new(tex.outputs['Fac'],bump.inputs['Height'])
        if bevel:
            m.node_tree.links.new(b.outputs['Normal'],bump.inputs['Normal'])
        m.node_tree.links.new(bump.outputs['Normal'],p.inputs['Normal'])
    return m

with zipfile.ZipFile(SOURCE) as z:
    root = ET.fromstring(z.read('3D/3dmodel.model'))
ns = {'m':'http://schemas.microsoft.com/3dmanufacturing/core/2015/02'}
base_colors = [e.get('displaycolor') for e in root.findall('m:resources/m:basematerials/m:base',ns)]
palette = []
for i,color in enumerate(base_colors):
    metal = .82 if i in (2,8,10,24,26) else 0
    rough = .27 if metal else .4
    palette.append(material('Assembly material %02d %s'%(i,color),color,rough,metal))

black_pcb = material('Black solder mask', '#171C20', .33)
top_pcb = black_pcb.copy()
top_pcb.name = 'Black PCB — actual KiCad silkscreen and pad apertures'
nodes = top_pcb.node_tree.nodes
links = top_pcb.node_tree.links
principled = nodes.get('Principled BSDF')
mask_tex = nodes.new('ShaderNodeTexImage')
mask_tex.image = bpy.data.images.load(str(OUT/'assets/keel_mask_apertures.png'))
mask_tex.image.pack()
mask_tex.extension='CLIP'
silk_tex = nodes.new('ShaderNodeTexImage')
silk_tex.image = bpy.data.images.load(str(OUT/'assets/keel_silkscreen_white.png'))
silk_tex.image.pack()
silk_tex.extension='CLIP'
pads = nodes.new('ShaderNodeMixRGB')
pads.inputs[1].default_value=(*rgb('#171C20'),1)
pads.inputs[2].default_value=(*rgb('#B99B58'),1)
links.new(mask_tex.outputs['Alpha'],pads.inputs[0])
silk_mix=nodes.new('ShaderNodeMixRGB')
links.new(pads.outputs[0],silk_mix.inputs[1])
silk_mix.inputs[2].default_value=(*rgb('#DDE2DE'),1)
links.new(silk_tex.outputs['Alpha'],silk_mix.inputs[0])
links.new(silk_mix.outputs[0],principled.inputs['Base Color'])
links.new(mask_tex.outputs['Alpha'],principled.inputs['Metallic'])
pcb_edge = material('PCB laminate edge', '#343626', .6, bevel=False)
petg = material('Satin graphite PETG', '#41484F', .39, texture=True)
rubber = material('Soft black tyre rubber', '#181A1C', .68, texture=True, bevel=False)
bank = material('Power bank — charcoal polymer', '#30363C', .43, texture=True)
black_metal = material('Black oxide fasteners', '#24292E', .32, .65)
aluminum = material('Satin aluminum transducer shell', '#BFC5CB', .28, .95)
grille = material('Woven stainless steel grille', '#AEB6BD', .34, .91, bevel=False)
dark = material('Recessed acoustic cavity', '#131A1E', .65, bevel=False)
silk = material('White silkscreen', '#D9DFDA', .6, bevel=False)

# Subtle real-world surface structure: layer lines on PETG and fine tyre tread.
def math_node(nodes,op,*values):
    n=nodes.new('ShaderNodeMath')
    n.operation=op
    for i,v in enumerate(values):
        if isinstance(v,(float,int)):
            n.inputs[i].default_value=v
        else:
            nodes.id_data.links.new(v,n.inputs[i])
    return n.outputs[0]

pn=petg.node_tree.nodes
pl=petg.node_tree.links
geo=pn.new('ShaderNodeNewGeometry')
xyz=pn.new('ShaderNodeSeparateXYZ')
pl.new(geo.outputs['Position'],xyz.inputs[0])
layer_height=math_node(pn,'SINE',math_node(pn,'MULTIPLY',xyz.outputs['Z'],2*math.pi/.0002))
layer=pn.new('ShaderNodeBump')
layer.inputs['Strength'].default_value=.22
layer.inputs['Distance'].default_value=.000025
pl.new(layer_height,layer.inputs['Height'])
pbs=pn.get('Principled BSDF')
existing=pbs.inputs['Normal'].links[0].from_socket
pl.new(existing,layer.inputs['Normal'])
pl.new(layer.outputs['Normal'],pbs.inputs['Normal'])

rn=rubber.node_tree.nodes
rl=rubber.node_tree.links
geo=rn.new('ShaderNodeNewGeometry')
xyz=rn.new('ShaderNodeSeparateXYZ')
rl.new(geo.outputs['Position'],xyz.inputs[0])
dy=math_node(rn,'SUBTRACT',xyz.outputs['Y'],.028002)
dz=math_node(rn,'SUBTRACT',xyz.outputs['Z'],.0235)
angle=math_node(rn,'ARCTAN2',dz,dy)
along=math_node(rn,'SUBTRACT',math_node(rn,'ABSOLUTE',xyz.outputs['X']),.061)
phase=math_node(rn,'ADD',math_node(rn,'MULTIPLY',angle,48),math_node(rn,'MULTIPLY',math_node(rn,'ABSOLUTE',along),1600))
wave=math_node(rn,'SINE',phase)
radius=math_node(rn,'SQRT',math_node(rn,'ADD',math_node(rn,'MULTIPLY',dy,dy),math_node(rn,'MULTIPLY',dz,dz)))
gate=math_node(rn,'GREATER_THAN',radius,.0224)
tread=rn.new('ShaderNodeBump')
tread.inputs['Strength'].default_value=.45
tread.inputs['Distance'].default_value=.00014
rl.new(math_node(rn,'MULTIPLY',wave,gate),tread.inputs['Height'])
rbs=rn.get('Principled BSDF')
existing=rbs.inputs['Normal'].links[0].from_socket
rl.new(existing,tread.inputs['Normal'])
rl.new(tread.outputs['Normal'],rbs.inputs['Normal'])

def mesh_object(name, verts, faces, mat, target=details, smooth=True):
    mesh = bpy.data.meshes.new(name)
    mesh.from_pydata(verts,[],faces)
    mesh.update()
    if smooth:
        for p in mesh.polygons:
            p.use_smooth = True
        mesh.set_sharp_from_angle(angle=math.radians(35))
    obj = bpy.data.objects.new(name,mesh)
    target.objects.link(obj)
    obj.data.materials.append(mat)
    return obj

count = 0
for item in root.findall('m:resources/m:object',ns):
    mesh = item.find('m:mesh',ns)
    if mesh is None:
        continue
    name = item.get('name','Part')
    # Replace only the simplified solid-can geometry in this derived scene.
    if name.startswith('HC-SR04 ultrasonic sensor: cans') or name == 'HC-SR04 ultrasonic sensor: crystal':
        continue
    verts = [tuple(float(v.get(c))*MM for c in 'xyz') for v in mesh.findall('m:vertices/m:vertex',ns)]
    faces = [tuple(int(t.get(c)) for c in ('v1','v2','v3')) for t in mesh.findall('m:triangles/m:triangle',ns)]
    mat = palette[int(item.get('pindex','0'))]
    if name == 'Keel R3 board: pcb':
        mat = black_pcb
    elif name.startswith('P') and len(name)>2 and name[1:3].isdigit():
        mat = petg
    elif ': tyre' in name:
        mat = rubber
    elif name.startswith('Power bank'):
        mat = bank
    elif 'socket head screw' in name:
        mat = black_metal
    obj = mesh_object(name,verts,faces,mat,robot)
    obj['source_3mf_name'] = name
    if name == 'Keel R3 board: pcb':
        obj.data.materials.append(pcb_edge)
        obj.data.materials.append(top_pcb)
        uv=obj.data.uv_layers.new(name='KiCad top projection')
        for loop in obj.data.loops:
            v=obj.data.vertices[loop.vertex_index].co
            uv.data[loop.index].uv=((v.x/MM+53)/106,(v.y/MM+75)/150)
        for p in obj.data.polygons:
            if abs(p.normal.z)<.5:
                p.material_index = 1
            elif p.normal.z>.5:
                p.material_index = 2
    count += 1

def lathe_y(name, cx, cz, profile, mat, segments=128):
    verts=[]
    faces=[]
    for y,r in profile:
        for i in range(segments):
            a=i*2*math.pi/segments
            verts.append(((cx+r*math.cos(a))*MM,y*MM,(cz+r*math.sin(a))*MM))
    for row in range(len(profile)-1):
        for i in range(segments):
            j=(i+1)%segments
            faces.append((row*segments+i,row*segments+j,(row+1)*segments+j,(row+1)*segments+i))
    return mesh_object(name,verts,faces,mat)

def wire(name,points,radius,mat):
    curve=bpy.data.curves.new(name,'CURVE')
    curve.dimensions='3D'
    curve.resolution_u=1
    curve.bevel_depth=radius*MM
    curve.bevel_resolution=2
    spline=curve.splines.new('POLY')
    spline.points.add(len(points)-1)
    for p,v in zip(spline.points,points):
        p.co=(*[c*MM for c in v],1)
    obj=bpy.data.objects.new(name,curve)
    details.objects.link(obj)
    curve.materials.append(mat)
    return obj

for index,cx in enumerate((-13,13),1):
    # Ø16 x 12 mm outer envelope, matching the original model and mount.
    profile=[(53.5,7.85),(53.65,8),(64.65,8),(65.15,7.90),
             (65.5,7.50),(65.5,6.85),(65.22,6.65),(64.80,6.65),
             (64.55,7.30),(54.0,7.30),(53.5,7.30),(53.5,7.85)]
    lathe_y('HC-SR04 %d — rolled aluminum can'%index,cx,48.1,profile,aluminum)
    lathe_y('HC-SR04 %d — dark inner wall'%index,cx,48.1,[(62.5,6.65),(64.7,6.65)],dark)
    lathe_y('HC-SR04 %d — recessed diaphragm'%index,cx,48.1,
            [(61.8,0),(61.8,2),(62.0,4.8),(62.7,6.4)],aluminum)
    # Interleaved wires: a real geometric grille with dark gaps and parallax.
    radius=6.65
    pitch=.50
    for axis in (0,1):
        for j in range(-13,14):
            offset=j*pitch
            if abs(offset)>=radius:
                continue
            half=math.sqrt(radius*radius-offset*offset)
            steps=max(4,math.ceil(half*2/.16))
            points=[]
            for k in range(steps+1):
                t=-half+2*half*k/steps
                # Rotate the weave 45 degrees, as on the reference component.
                u,v=(t,offset) if axis==0 else (offset,t)
                x=(u-v)/math.sqrt(2)
                z=(u+v)/math.sqrt(2)
                y=64.82 + .035*math.cos(math.pi*t/pitch)*((-1)**j)*(1 if axis==0 else -1)
                points.append((cx+x,y,48.1+z))
            wire('HC-SR04 %d — grille %d %d'%(index,axis,j),points,.045,grille)
    lathe_y('HC-SR04 %d — rolled seam'%index,cx,48.1,
            [(64.25,7.995),(64.30,8.01),(64.38,8.01),(64.43,7.995)],aluminum)

def label_front(name,text,x,y,z,size):
    data=bpy.data.curves.new(name,'FONT')
    data.body=text
    data.size=size*MM
    data.align_x='CENTER'
    data.align_y='CENTER'
    data.extrude=0
    obj=bpy.data.objects.new(name,data)
    details.objects.link(obj)
    obj.location=(x*MM,y*MM,z*MM)
    obj.rotation_euler=(math.pi/2,0,math.pi)
    data.materials.append(silk)
    return obj

def capsule(name,length,height,y0,y1,mat):
    radius=height/2
    half_straight=length/2-radius
    outline=[]
    for center,start in ((half_straight,-math.pi/2),(-half_straight,math.pi/2)):
        for i in range(49):
            a=start+i*math.pi/48
            outline.append((center+radius*math.cos(a),55.6+radius*math.sin(a)))
    n=len(outline)
    verts=[(x*MM,y*MM,z*MM) for y in (y0,y1) for x,z in outline]
    faces=[tuple(range(n-1,-1,-1)),tuple(range(n,2*n))]
    faces.extend((i,(i+1)%n,(i+1)%n+n,i+n) for i in range(n))
    return mesh_object(name,verts,faces,mat)

capsule('HC-SR04 — crystal stamped base flange',11,4.6,53.5,53.85,aluminum)
capsule('HC-SR04 — rounded crystal can',10.55,4.18,53.85,56.9,aluminum)
crystal_text=label_front('Crystal frequency marking','8.000',0,56.915,55.6,.75)
crystal_text.data.materials.clear()
crystal_text.data.materials.append(black_metal)
label_front('HC-SR04 model marking','HC-SR04',0,53.515,51.5,1.2)
for x,text in zip((-3.81,-1.27,1.27,3.81),('Vcc','Trig','Echo','Gnd')):
    label_front('HC-SR04 pin '+text,text,x,53.515,41.1,.78)

# Add a coated optical surface to the camera's existing barrel.
glass=material('Camera lens — coated optical glass','#152B37',.08,.25,bevel=False)
pp=glass.node_tree.nodes.get('Principled BSDF')
pp.inputs['Coat Weight'].default_value=.9
pp.inputs['Coat Roughness'].default_value=.04
lathe_y('Camera — recessed lens glass',16.5,72.1,
        [(56.96,0),(56.95,.8),(56.90,1.5),(56.80,2.12)],glass)
lathe_y('Camera — lens retaining ring',16.5,72.1,
        [(56.81,2.13),(57.03,2.20),(57.03,2.60),(56.85,2.80)],black_metal)

# Stage is deliberately oversized so there is no visible horizon.
floor=material('Studio sweep — warm light grey','#DFE1E3',.6,bevel=False)
floor_bs= floor.node_tree.nodes.get('Principled BSDF')
floor_bs.inputs['Emission Color'].default_value=(*rgb('#CDD0D2'),1)
floor_bs.inputs['Emission Strength'].default_value=.16
mesh_object('Seamless studio floor',[(-200,-200,-.00012),(200,-200,-.00012),
            (200,200,-.00012),(-200,200,-.00012)],[(0,1,2,3)],floor,studio,False)
world=bpy.data.worlds.new('Soft neutral ambient')
world.use_nodes=True
world.node_tree.nodes['Background'].inputs[0].default_value=(.65,.72,.83,1)
world.node_tree.nodes['Background'].inputs[1].default_value=.25
scene.world=world

def aim(obj,target):
    obj.rotation_euler=(Vector(target)-obj.location).to_track_quat('-Z','Y').to_euler()

def area(name,location,power,size,color=(1,1,1),size_y=None):
    data=bpy.data.lights.new(name,'AREA')
    data.energy=power
    data.color=color
    data.shape='RECTANGLE'
    data.size=size
    data.size_y=size_y or size
    obj=bpy.data.objects.new(name,data)
    studio.objects.link(obj)
    obj.location=location
    aim(obj,(0,-.015,.04))
    return obj

area('Key — large left softbox',(-.26,.19,.38),3,.32,(1,.95,.89),.24)
area('Fill — front right softbox',(.28,.20,.22),1.2,.24,(.87,.93,1),.30)
area('Rim — overhead rear strip',(.08,-.25,.34),4,.28,(1,1,1),.12)
area('Front — transducer reflection card',(-.04,.33,.11),.3,.18,(1,1,1),.08)

cam_data=bpy.data.cameras.new('Product camera — 70 mm')
camera=bpy.data.objects.new('Product camera — front right',cam_data)
studio.objects.link(camera)
camera.location=(.285,.390,.255)
aim(camera,(0,-.020,.040))
cam_data.lens=75
cam_data.clip_start=.001
cam_data.clip_end=1000
scene.camera=camera

scene.render.engine='CYCLES'
prefs=bpy.context.preferences.addons['cycles'].preferences
try:
    prefs.compute_device_type='OPTIX'
    prefs.get_devices()
    for d in prefs.devices:
        d.use=d.type=='OPTIX'
    scene.cycles.device='GPU'
except Exception as exc:
    print('GPU fallback:',exc)
    scene.cycles.device='CPU'
scene.cycles.samples=32 if PREVIEW else 256
scene.cycles.use_denoising=True
scene.cycles.adaptive_threshold=.025 if PREVIEW else .008
scene.cycles.max_bounces=10
scene.cycles.diffuse_bounces=4
scene.cycles.glossy_bounces=6
scene.render.resolution_x=1200 if PREVIEW else 3840
scene.render.resolution_y=900 if PREVIEW else 2880
scene.render.resolution_percentage=100
from bpy_extras.object_utils import world_to_camera_view
bpy.context.view_layer.update()
projected=[world_to_camera_view(scene,camera,obj.matrix_world @ Vector(corner)) for obj in robot.objects for corner in obj.bound_box]
cam_data.shift_x=(min(v.x for v in projected)+max(v.x for v in projected))/2-.5
cam_data.shift_y=((min(v.y for v in projected)+max(v.y for v in projected))/2-.5)*scene.render.resolution_y/scene.render.resolution_x
scene.render.image_settings.file_format='PNG'
scene.render.image_settings.color_mode='RGB'
scene.render.image_settings.color_depth='8'
scene.render.film_transparent=False
scene.view_settings.view_transform='AgX'
scene.view_settings.look='AgX - Medium High Contrast'
scene.view_settings.exposure=-.25
scene.render.filepath=str(OUT/('preview.png' if PREVIEW else 'Keel_R3_studio.png'))
scene['Source assembly']=str(SOURCE)
scene['Rendering notes']='Original assembly geometry; black base PCB; graphite PETG; HC-SR04 cans refined within original Ø16×12mm envelope. Grille dimensions are visual estimates.'
print('Imported mesh parts:',count,'; detailed sensor geometry:',len(details.objects))
if not PREVIEW:
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT/'Keel_R3_studio.blend'))
bpy.ops.render.render(write_still=True)
print('RENDER_COMPLETE',scene.render.filepath)
