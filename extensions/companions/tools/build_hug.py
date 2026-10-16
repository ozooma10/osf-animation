"""Generate prototype entrance/release clips from the accepted Contact v2 pair.

Run in Blender. The held hug is unchanged. Transitions are authored here with
smooth timing, arm IK and planted/stepping foot targets, then saved as GLBs and
an editable Blender preview. This is a prototype motion pass, not mesh collision
validation. Original Sleep Intimate/HKX files are not embedded in this script.
"""
from copy import deepcopy
from pathlib import Path
import hashlib
import json
import math
import sys

ROOT = Path(__file__).resolve().parents[3]
ADDON = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(Path(__file__).resolve().parent), str(ROOT/'tools/skyrim-import')]
from mathutils import Quaternion, Vector
from contact_adjust import adjust_wrist, _models, _write_world_rotation
from glb_writer import write_glb
from preview_audit import _clip
from target_rig import read_rig, validate_glb
from worker import save_preview

SOURCE = ROOT/'build/hug-animation-research/output/SleepIntimateHugs-Contact-v2'
EXPECTED = {'F': '5fd1cf6cc3b0f2181bcfb77f9fec4944b3dea8f5030bbf88b0a0072657d2bdbd',
            'M': 'b9aacd33eaf6d2797661e956e0f0545340812c38f0cfc482b427ba93d56af13d'}
OUT = ADDON/'dist'
AUTHORING = ROOT/'build/companion-hug'
FPS = 30
ENTER = 2.8
EXIT = 2.8


def smooth(x):
    x = max(0., min(1., x))
    return x*x*x*(10+x*(-15+6*x))


def quaternion(values): return Quaternion((values[3], *values[:3]))
def xyzw(q): return [q.x, q.y, q.z, q.w]


def foot_ik(frame, names, parents, side, target, hint):
    """Two-bone leg solve preserving segment lengths and foot orientation."""
    a, b, c = [names.index(f'{side}_{part}') for part in ('Thigh','Calf','Foot')]
    m = _models(frame, parents)
    start, knee, foot = [m[i].translation for i in (a,b,c)]
    upper, lower = (knee-start).length, (foot-knee).length
    axis = target-start
    length = axis.length
    if not abs(upper-lower)+1e-6 < length < upper+lower-1e-6:
        raise ValueError(f'Unreachable {side} foot: {length} vs {upper+lower}')
    axis.normalize()
    pole = hint-start
    pole -= axis*pole.dot(axis)
    pole.normalize()
    axial = (upper*upper-lower*lower+length*length)/(2*length)
    new_knee = start+axis*axial+pole*math.sqrt(max(0., upper*upper-axial*axial))
    _write_world_rotation(frame,m,parents,a,(knee-start).rotation_difference(new_knee-start) @ m[a].to_quaternion())
    second = _models(frame,parents)
    _write_world_rotation(frame,second,parents,b,(second[c].translation-second[b].translation).rotation_difference(target-second[b].translation) @ second[b].to_quaternion())
    third = _models(frame,parents)
    _write_world_rotation(frame,third,parents,c,m[c].to_quaternion())
    actual = _models(frame,parents)[c].translation
    assert (actual-target).length < 2e-5
    return (actual-target).length


def transition(contact, role, rig, progress, arm_keys=None):
    """0 = open standing pose, 1 = exact accepted hug endpoint."""
    if progress >= 1.: return deepcopy(contact), 0.
    frame = deepcopy(contact)
    names = [b.name for b in rig.bones]
    parents = [b.parent for b in rig.bones]
    com = names.index('COM')
    direction = 1. if role == 'F' else -1.
    # Sarah takes a small two-foot step. The player's feet remain in place.
    travel = -.26 if role == 'F' else 0.
    body_progress = smooth((progress-.3)/.6)
    frame[com]['translation'][1] += travel*(1-body_progress)
    if role == 'F':
        frame[com]['translation'][2] -= .035*math.sin(math.pi*progress)**2
    torso = smooth(progress/.65)
    for bone in ('C_Spine1','C_Spine2','C_Chest','C_Neck','C_Neck1','C_Head'):
        i = names.index(bone)
        frame[i]['rotation'] = xyzw(quaternion(rig.bones[i].rotation_xyzw).slerp(quaternion(contact[i]['rotation']), torso))
    contact_models = _models(contact,parents)
    for side in ('L','R'):
        m = _models(frame,parents)
        shoulder = m[names.index(side+'_Biceps')].translation
        contact_shoulder = contact_models[names.index(side+'_Biceps')].translation
        handed = (-1. if side == 'L' else 1.)*direction
        relaxed = shoulder + Vector((handed*.09, direction*.025, -.46))
        open_hand = shoulder + Vector((handed*.38, direction*.10, -.18))
        destination = contact_models[names.index(side+'_Wrist')].translation
        # Move around the partner's sides before bringing the arms inward.
        if progress < .45:
            wrist = relaxed.lerp(open_hand, smooth(progress/.45))
        else:
            wrist = open_hand.lerp(destination, smooth((progress-.45)/.55))
        elbow_relaxed = shoulder + Vector((handed*.19, -direction*.02, -.27))
        elbow_contact = contact_models[names.index(side+'_Forearm')].translation
        elbow = elbow_relaxed.lerp(elbow_contact, smooth(progress))
        frame, metrics = adjust_wrist(rig,frame,side,target_position=wrist,elbow_hint=elbow)
        wrist_index = names.index(side+'_Wrist')
        frame[wrist_index]['rotation'] = xyzw(quaternion(rig.bones[wrist_index].rotation_xyzw).slerp(quaternion(contact[wrist_index]['rotation']), smooth(progress)))
        for i, bone in enumerate(rig.bones):
            if bone.name.startswith(tuple(side+'_'+digit for digit in ('Thumb','Index','Middle','Ring','Pinky'))):
                frame[i]['rotation'] = xyzw(quaternion(bone.rotation_xyzw).slerp(quaternion(contact[i]['rotation']), smooth(progress)))
    if arm_keys is not None:
        # Interpolate authored IK key poses in joint space. Solving an elbow pole
        # independently every frame can flip as the wrist crosses the shoulder.
        before, after = (arm_keys[0], arm_keys[1]) if progress < .45 else (arm_keys[1], arm_keys[2])
        weight = smooth((progress-before[0])/(after[0]-before[0]))
        for side in ('L','R'):
            for part in ('Biceps','Forearm','Wrist'):
                i=names.index(side+'_'+part)
                frame[i]['rotation']=xyzw(quaternion(before[1][i]['rotation']).slerp(quaternion(after[1][i]['rotation']),weight))
    error = 0.
    if role == 'F':
        for side, begin, end in (('L',.25,.6),('R',.55,.9)):
            step = max(0.,min(1.,(progress-begin)/(end-begin)))
            target = contact_models[names.index(side+'_Foot')].translation.copy()
            target.y += travel*(1-smooth(step))
            target.z += .045*math.sin(math.pi*step)**2
            hint = contact_models[names.index(side+'_Calf')].translation + Vector((0,travel*(1-body_progress)+.15,0))
            error = max(error,foot_ik(frame,names,parents,side,target,hint))
    return frame,error


def main():
    rig = read_rig(ROOT/'build/skyrim-import/target-rig/skeleton.rig')
    names = [b.name for b in rig.bones]
    bones = [dict(name=b.name,parent=b.parent,translation=list(b.translation),rotation=list(b.rotation_xyzw)) for b in rig.bones]
    base_scene = json.loads((SOURCE/'Data/OSF/sleepintimate-hugs.osf.json').read_text())
    scene = {**base_scene, 'id':'osf.companions.sarah.hug', 'name':'A hug with Sarah',
             'pack':'OSF Companions', 'tags':['hug','affection','companion','prototype'],
             'stages':[]}
    timeline = {}
    placements = []
    player_origin = Vector((0., .37134861946105957, 0.))
    report = {'status':'offline_checks_passed','held_hug':'Contact v2 unchanged',
              'transition_authoring':'procedural prototype; visual acceptance required', 'actors':[]}
    for role in ('F','M'):
        source = SOURCE/f'Data/OSF/Imported/SleepIntimateHugs/Embrace{role}.glb'
        assert hashlib.sha256(source.read_bytes()).hexdigest() == EXPECTED[role]
        nodes,parents,held_times,channels = _clip(source)
        assert [n['name'] for n in nodes] == names
        held = [[{k:list(channels[i,k][f]) for k in ('translation','rotation','scale')} for i in range(len(names))] for f in range(len(held_times))]
        enter_times = [i/FPS for i in range(round(ENTER*FPS)+1)]
        exit_times = [i/FPS for i in range(round(EXIT*FPS)+1)]
        enter_keys = [(p,transition(held[0],role,rig,p)[0]) for p in (0.,.45,1.)]
        exit_keys = [(p,transition(held[-1],role,rig,p)[0]) for p in (0.,.45,1.)]
        enter_pairs = [transition(held[0],role,rig,t/ENTER,enter_keys) for t in enter_times]
        exit_pairs = [transition(held[-1],role,rig,1-t/EXIT,exit_keys) for t in exit_times]
        enter,leave = [p[0] for p in enter_pairs],[p[0] for p in exit_pairs]
        assert enter[-1] == held[0] and leave[0] == held[-1]
        # Move the baked idle root into scene placement. Both capsules now end
        # where their visible bodies stand, facing each other after playback.
        com = names.index('COM')
        origin = Vector(enter[0][com]['translation']); origin.z = 0.
        authored_offset = Vector((-.03,-.02,0.)) if role == 'F' else Vector((0.,0.,0.))
        placement = origin + authored_offset - player_origin
        heading = 0. if role == 'F' else 180.
        placements.append(dict(zip(('x','y','z','heading'),(*placement,heading))))
        inverse = Quaternion((0.,0.,1.), -math.radians(heading))
        for frames in (enter,held,leave):
            for frame in frames:
                before = _models(frame,parents)
                frame[com]['translation'] = list(inverse @ (Vector(frame[com]['translation'])-origin))
                frame[com]['rotation'] = xyzw(inverse @ quaternion(frame[com]['rotation']))
                after = _models(frame,parents)
                forward = inverse.inverted()
                for i in range(len(names)):
                    if names[i] in ('L_Wrist','R_Wrist','L_Foot','R_Foot','C_Head'):
                        expected = before[i].translation+authored_offset-player_origin
                        actual = forward @ after[i].translation+placement
                        assert (actual-expected).length < 3e-6
        actor_report = {'role':role,'source_sha256':EXPECTED[role],
                        'max_foot_ik_error_m':max(p[1] for p in enter_pairs+exit_pairs),'clips':[]}
        for label,frames,times in (('Enter',enter,enter_times),('Hug',held,held_times),('Exit',leave,exit_times)):
            path=OUT/f'OSF/Companions/{label}{role}.glb'
            path.parent.mkdir(parents=True,exist_ok=True)
            write_glb(path,bones,times,frames,label+role)
            validate_glb(path)
            actor_report['clips'].append(dict(file=str(path.relative_to(OUT)),frames=len(times),duration=times[-1],sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
        timeline[role] = enter + held[1:] + leave[1:]
        report['actors'].append(actor_report)
    for label in ('Enter','Hug','Exit'):
        scene['stages'].append({'clips':[dict(file=f'OSF/Companions/{label}{role}.glb',offset=offset) for role,offset in zip(('F','M'),placements)],'loops':1})
    (OUT/'OSF/companions.osf.json').write_text(json.dumps(scene,indent=2)+'\n')
    full_times = enter_times + [ENTER+t for t in held_times[1:]] + [ENTER+held_times[-1]+t for t in exit_times[1:]]
    save_preview(AUTHORING/'Sarah-hug.blend',rig,scene['roles'],[timeline[r] for r in ('F','M')],full_times,placements)
    (AUTHORING/'animation-report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report))


if __name__ == '__main__': main()
