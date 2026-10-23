#!/usr/bin/env python3
# Writes the GameMaker project for gmRunner's feature tests (tests/gmtest/gmtest/): GameMaker's
# project files are plain JSON, so the project is generated from the GML in tests/gmtest/src/.
# Build it with GameMaker (Run in the IDE gives the reference results; tools/gmtest.sh builds
# it with Igor and compares gmRunner's results with those).
import os, shutil

here = os.path.dirname(os.path.abspath(__file__))
src = os.path.join(here, "src")
out = os.path.join(here, "gmtest")
name = "gmtest"

shutil.rmtree(out, ignore_errors=True)
os.makedirs(out)

def write(path, text):
    full = os.path.join(out, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "w") as f:
        f.write(text)

parent = '{"name":"%s","path":"%s.yyp",}' % (name, name)
resources, rooms = [], []

# Scripts: every src/scripts/*.gml becomes a script asset.
for fn in sorted(os.listdir(os.path.join(src, "scripts"))):
    s = fn[:-4]
    write("scripts/%s/%s.gml" % (s, s), open(os.path.join(src, "scripts", fn)).read())
    write("scripts/%s/%s.yy" % (s, s),
          '{"$GMScript":"v1","%%Name":"%s","isCompatibility":false,"isDnD":false,"name":"%s","parent":%s,'
          '"resourceType":"GMScript","resourceVersion":"2.0",}' % (s, s, parent))
    resources.append((s, "scripts/%s/%s.yy" % (s, s)))

# Objects: src/objects/<obj>/<Event>_<n>.gml (Create_0, Step_0, Draw_0, Other_4 ...).
EVENTS = {"Create": 0, "Destroy": 1, "Alarm": 2, "Step": 3, "Draw": 8, "Other": 7, "CleanUp": 12}
for o in sorted(os.listdir(os.path.join(src, "objects"))):
    events = []
    for fn in sorted(os.listdir(os.path.join(src, "objects", o))):
        kind, num = fn[:-4].split("_")
        write("objects/%s/%s" % (o, fn), open(os.path.join(src, "objects", o, fn)).read())
        events.append('{"$GMEvent":"v1","%%Name":"","collisionObjectId":null,"eventNum":%s,"eventType":%d,'
                      '"isDnD":false,"name":"","resourceType":"GMEvent","resourceVersion":"2.0",}' % (num, EVENTS[kind]))
    write("objects/%s/%s.yy" % (o, o), """{
  "$GMObject":"",
  "%%Name":"%s",
  "eventList":[
    %s
  ],
  "managed":true,
  "name":"%s",
  "overriddenProperties":[],
  "parent":%s,
  "parentObjectId":null,
  "persistent":false,
  "physicsAngularDamping":0.1,
  "physicsDensity":0.5,
  "physicsFriction":0.2,
  "physicsGroup":1,
  "physicsKinematic":false,
  "physicsLinearDamping":0.1,
  "physicsObject":false,
  "physicsRestitution":0.1,
  "physicsSensor":false,
  "physicsShape":1,
  "physicsShapePoints":[],
  "physicsStartAwake":true,
  "properties":[],
  "resourceType":"GMObject",
  "resourceVersion":"2.0",
  "solid":false,
  "spriteId":null,
  "spriteMaskId":null,
  "visible":true,
}""" % (o, ",\n    ".join(events), o, parent))
    resources.append((o, "objects/%s/%s.yy" % (o, o)))

# Rooms: src/rooms.txt, one room per line: name objects...
for line in open(os.path.join(src, "rooms.txt")):
    parts = line.split()
    if not parts or parts[0].startswith("#"):
        continue
    r, objs = parts[0], parts[1:]
    insts, order = [], []
    for i, o in enumerate(objs):
        iname = "inst_%s_%d" % (r, i)
        insts.append('{"$GMRInstance":"v4","%%Name":"%s","colour":4294967295,"frozen":false,"hasCreationCode":false,'
                     '"ignore":false,"imageIndex":0,"imageSpeed":1.0,"inheritCode":false,"inheritedItemId":null,'
                     '"inheritItemSettings":false,"isDnd":false,"name":"%s","objectId":{"name":"%s","path":"objects/%s/%s.yy",},'
                     '"properties":[],"resourceType":"GMRInstance","resourceVersion":"2.0","rotation":0.0,"scaleX":1.0,'
                     '"scaleY":1.0,"x":0.0,"y":0.0,}' % (iname, iname, o, o, o))
        order.append('{"name":"%s","path":"rooms/%s/%s.yy",}' % (iname, r, r))
    view = ('{"hborder":32,"hport":540,"hspeed":-1,"hview":540,"inherit":false,"objectId":null,"vborder":32,"visible":false,'
            '"vspeed":-1,"wport":960,"wview":960,"xport":0,"xview":0,"yport":0,"yview":0,}')
    write("rooms/%s/%s.yy" % (r, r), """{
  "$GMRoom":"v1",
  "%%Name":"%s",
  "creationCodeFile":"",
  "inheritCode":false,
  "inheritCreationOrder":false,
  "inheritLayers":false,
  "instanceCreationOrder":[%s],
  "isDnd":false,
  "layers":[
    {"$GMRInstanceLayer":"","%%Name":"Instances","depth":0,"effectEnabled":true,"effectType":null,"gridX":32,"gridY":32,"hierarchyFrozen":false,"inheritLayerDepth":false,"inheritLayerSettings":false,"inheritSubLayers":true,"inheritVisibility":true,"instances":[%s],"layers":[],"name":"Instances","properties":[],"resourceType":"GMRInstanceLayer","resourceVersion":"2.0","userdefinedDepth":false,"visible":true,},
    {"$GMRBackgroundLayer":"","%%Name":"Background","animationFPS":15.0,"animationSpeedType":0,"colour":4281545523,"depth":100,"effectEnabled":true,"effectType":null,"gridX":32,"gridY":32,"hierarchyFrozen":false,"hspeed":0.0,"htiled":false,"inheritLayerDepth":false,"inheritLayerSettings":false,"inheritSubLayers":true,"inheritVisibility":true,"layers":[],"name":"Background","properties":[],"resourceType":"GMRBackgroundLayer","resourceVersion":"2.0","spriteId":null,"stretch":false,"userdefinedAnimFPS":false,"userdefinedDepth":false,"visible":true,"vspeed":0.0,"vtiled":false,"x":0,"y":0,},
  ],
  "name":"%s",
  "parent":%s,
  "parentRoom":null,
  "physicsSettings":{"inheritPhysicsSettings":false,"PhysicsWorld":false,"PhysicsWorldGravityX":0.0,"PhysicsWorldGravityY":10.0,"PhysicsWorldPixToMetres":0.1,},
  "resourceType":"GMRoom",
  "resourceVersion":"2.0",
  "roomSettings":{"Height":540,"inheritRoomSettings":false,"persistent":false,"Width":960,},
  "sequenceId":null,
  "views":[%s],
  "viewSettings":{"clearDisplayBuffer":true,"clearViewBackground":false,"enableViews":false,"inheritViewSettings":false,},
  "volume":1.0,
}""" % (r, ",".join(order), ",".join(insts), r, parent, ",".join([view] * 8)))
    resources.append((r, "rooms/%s/%s.yy" % (r, r)))
    rooms.append(r)

# Included files: src/datafiles/* go to the game's working directory.
included = []
dsrc = os.path.join(src, "datafiles")
if os.path.isdir(dsrc):
    for fn in sorted(os.listdir(dsrc)):
        os.makedirs(os.path.join(out, "datafiles"), exist_ok=True)
        shutil.copy(os.path.join(dsrc, fn), os.path.join(out, "datafiles", fn))
        included.append('{"$GMIncludedFile":"","%%Name":"%s","CopyToMask":-1,"filePath":"datafiles","name":"%s",'
                        '"resourceType":"GMIncludedFile","resourceVersion":"2.0",}' % (fn, fn))

res = "\n".join('    {"id":{"name":"%s","path":"%s",},},' % r for r in resources)
order = "\n".join('    {"roomId":{"name":"%s","path":"rooms/%s/%s.yy",},},' % (r, r, r) for r in rooms)
write(name + ".yyp", """{
  "$GMProject":"",
  "%%Name":"%s",
  "AudioGroups":[
    {"$GMAudioGroup":"","%%Name":"audiogroup_default","name":"audiogroup_default","resourceType":"GMAudioGroup","resourceVersion":"2.0","targets":-1,},
  ],
  "configs":{"children":[],"name":"Default",},
  "defaultScriptType":0,
  "Folders":[],
  "IncludedFiles":[
%s
  ],
  "isEcma":false,
  "LibraryEmitters":[],
  "MetaData":{"IDEVersion":"2023.200.0.299",},
  "name":"%s",
  "resources":[
%s
  ],
  "resourceType":"GMProject",
  "resourceVersion":"2.0",
  "RoomOrderNodes":[
%s
  ],
  "templateType":null,
  "TextureGroups":[
    {"$GMTextureGroup":"","%%Name":"Default","autocrop":true,"border":2,"compressFormat":"bz2","customOptions":"","directory":"","groupParent":null,"isScaled":true,"loadType":"default","mipsToGenerate":0,"name":"Default","resourceType":"GMTextureGroup","resourceVersion":"2.0","targets":-1,},
  ],
}""" % (name, "\n".join("    " + i for i in included), name, res, order))
print("wrote", out)
