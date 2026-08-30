from pathlib import Path

HERE = Path(__file__).resolve().parent
base = HERE / "apply_v319_static_instancing.py"
source = base.read_text(encoding="utf-8")

# V3.12 replaces the pristine one-line attachTo selection with mutable spatial
# routing; V3.15 extends that same generated block with packetized routing. P1
# runs after both layers, so preserve their routing verbatim and anchor only on
# the invariant tail shared by the generated source.
old_anchor = '''attach_anchor = r\'''                osg::Group* const attachTo = merge ? mergeGroup : group;
                attachTo->addChild(trans);
                ++numinstances;
            }
            if (numinstances > 0)
\'''\n'''
new_anchor = '''attach_anchor = r\'''                attachTo->addChild(trans);
                ++numinstances;
            }
            if (numinstances > 0)
\'''\n'''
if source.count(old_anchor) != 1:
    raise RuntimeError(f"V3.19 P1 compat attach-anchor source mismatch: {source.count(old_anchor)}")
source = source.replace(old_anchor, new_anchor, 1)

old_replacement = '''    r\'''                osg::Group* const attachTo = merge ? mergeGroup : group;
                attachTo->addChild(trans);

                if (v319CandidatePair && ref.mScale > 0.f)
'''
new_replacement = '''    r\'''                attachTo->addChild(trans);

                if (v319CandidatePair && ref.mScale > 0.f)
'''
if source.count(old_replacement) != 1:
    raise RuntimeError(f"V3.19 P1 compat attach-replacement source mismatch: {source.count(old_replacement)}")
source = source.replace(old_replacement, new_replacement, 1)

# The two count==1 guards above are the compatibility invariant. Execute the
# corrected layer only after both exact source rewrites have succeeded.
exec(compile(source, str(base), "exec"), {"__file__": str(base), "__name__": "__main__"})
print("V3.19 P1 generated ObjectPaging routing compatibility applied")
