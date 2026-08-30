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

# The runtime-mode layer writes the same "$V319OsgThreading = ''" substring
# into the defaults block and several switch cases. The original P1 count guard
# therefore sees multiple matches even though the defaults assignment is valid.
# Scope the insertion to the unique two-line defaults block instead.
old_defaults = '''osg_default = "$V319OsgThreading = ''"
if text.count(osg_default) != 1:
    raise RuntimeError("V3.19 P1 launcher defaults mismatch")
text = text.replace(osg_default, osg_default + "\\n$V319StaticInstancing = '0'", 1)
'''
new_defaults = '''osg_default = "$V319FocusCadence = '1'\\n$V319OsgThreading = ''"
if text.count(osg_default) != 1:
    raise RuntimeError("V3.19 P1 launcher defaults mismatch")
text = text.replace(osg_default, osg_default + "\\n$V319StaticInstancing = '0'", 1)
'''
if source.count(old_defaults) != 1:
    raise RuntimeError(f"V3.19 P1 compat launcher-default source mismatch: {source.count(old_defaults)}")
source = source.replace(old_defaults, new_defaults, 1)

# Exact-count guards above are the compatibility invariant. Execute the
# corrected P1 layer only after all generated-source rewrites have succeeded.
exec(compile(source, str(base), "exec"), {"__file__": str(base), "__name__": "__main__"})
print("V3.19 P1 generated ObjectPaging/launcher compatibility applied; full-build gate ready")
