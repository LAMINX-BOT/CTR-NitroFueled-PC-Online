#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Adds Arabic Presentation Forms-B code points (U+FE70-U+FEFC) to a font's cmap.

The launcher (launcher/windows/arabic.cpp) shapes Arabic itself, as presentation forms, because
Dear ImGui has no shaping engine. Many fonts draw the contextual forms only through their
OpenType features (isol/init/medi/fina, lam-alef ligatures) and do not map every presentation
code point. This maps the missing ones to the glyphs those features already use; outlines and
metrics are unchanged.

    python tools/arabic_presentation_forms.py <in.ttf> <out.ttf> [--weight 700] [--family NAME]
(needs fontTools; --weight instances a variable font first)
"""
import argparse
import sys

from fontTools.ttLib import TTFont

# letter: (isolated, final, initial, medial); 0 = no such form.
FORMS = {
    0x0621: (0xFE80, 0, 0, 0), 0x0622: (0xFE81, 0xFE82, 0, 0), 0x0623: (0xFE83, 0xFE84, 0, 0),
    0x0624: (0xFE85, 0xFE86, 0, 0), 0x0625: (0xFE87, 0xFE88, 0, 0), 0x0626: (0xFE89, 0xFE8A, 0xFE8B, 0xFE8C),
    0x0627: (0xFE8D, 0xFE8E, 0, 0), 0x0628: (0xFE8F, 0xFE90, 0xFE91, 0xFE92), 0x0629: (0xFE93, 0xFE94, 0, 0),
    0x062A: (0xFE95, 0xFE96, 0xFE97, 0xFE98), 0x062B: (0xFE99, 0xFE9A, 0xFE9B, 0xFE9C),
    0x062C: (0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0), 0x062D: (0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4),
    0x062E: (0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8), 0x062F: (0xFEA9, 0xFEAA, 0, 0), 0x0630: (0xFEAB, 0xFEAC, 0, 0),
    0x0631: (0xFEAD, 0xFEAE, 0, 0), 0x0632: (0xFEAF, 0xFEB0, 0, 0), 0x0633: (0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4),
    0x0634: (0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8), 0x0635: (0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC),
    0x0636: (0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0), 0x0637: (0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4),
    0x0638: (0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8), 0x0639: (0xFEC9, 0xFECA, 0xFECB, 0xFECC),
    0x063A: (0xFECD, 0xFECE, 0xFECF, 0xFED0), 0x0641: (0xFED1, 0xFED2, 0xFED3, 0xFED4),
    0x0642: (0xFED5, 0xFED6, 0xFED7, 0xFED8), 0x0643: (0xFED9, 0xFEDA, 0xFEDB, 0xFEDC),
    0x0644: (0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0), 0x0645: (0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4),
    0x0646: (0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8), 0x0647: (0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC),
    0x0648: (0xFEED, 0xFEEE, 0, 0), 0x0649: (0xFEEF, 0xFEF0, 0, 0), 0x064A: (0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4),
}
# lam + alef: (alef, isolated ligature, final ligature)
LAM_ALEF = [(0x0622, 0xFEF5, 0xFEF6), (0x0623, 0xFEF7, 0xFEF8), (0x0625, 0xFEF9, 0xFEFA), (0x0627, 0xFEFB, 0xFEFC)]


def single_subs(font, feature):
    """glyph -> glyph of the single substitutions under an OpenType feature (arab script)."""
    gsub = font['GSUB'].table if 'GSUB' in font else None
    out = {}
    if not gsub or not gsub.FeatureList:
        return out
    for rec in gsub.FeatureList.FeatureRecord:
        if rec.FeatureTag != feature:
            continue
        for index in rec.Feature.LookupListIndex:
            lookup = gsub.LookupList.Lookup[index]
            for sub in lookup.SubTable:
                if lookup.LookupType == 7:
                    sub = sub.ExtSubTable
                if hasattr(sub, 'mapping'):
                    for a, b in sub.mapping.items():
                        out.setdefault(a, b)
    return out


def ligatures(font):
    """(first glyph, second glyph) -> ligature glyph, from every ligature lookup."""
    gsub = font['GSUB'].table if 'GSUB' in font else None
    out = {}
    if not gsub:
        return out
    for lookup in gsub.LookupList.Lookup:
        for sub in lookup.SubTable:
            if lookup.LookupType == 7:
                sub = sub.ExtSubTable
            if getattr(sub, 'LookupType', lookup.LookupType) == 4 and hasattr(sub, 'ligatures'):
                for first, ligs in sub.ligatures.items():
                    for lig in ligs:
                        if len(lig.Component) == 1:
                            out.setdefault((first, lig.Component[0]), lig.LigGlyph)
    return out


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('input')
    p.add_argument('output')
    p.add_argument('--weight', type=float, help='instance a variable font at this weight first')
    p.add_argument('--family', help='new family name (an OFL modified version keeps its own name)')
    a = p.parse_args()
    font = TTFont(a.input)
    if a.weight and 'fvar' in font:
        from fontTools.varLib import instancer
        font = instancer.instantiateVariableFont(font, {'wght': a.weight})
    cmap = font.getBestCmap()
    feats = {f: single_subs(font, f) for f in ('isol', 'fina', 'init', 'medi')}
    added = missing = 0
    new = {}
    for letter, forms in FORMS.items():
        base = cmap.get(letter)
        if not base:
            continue
        for form, feature in zip(forms, ('isol', 'fina', 'init', 'medi')):
            if not form or form in cmap:
                continue
            glyph = feats[feature].get(base, base if feature == 'isol' else None)
            if glyph:
                new[form] = glyph
                added += 1
            else:
                missing += 1
    ligs = ligatures(font)
    lam = cmap.get(0x0644)
    for alef, iso, fin in LAM_ALEF:
        if not lam or alef not in cmap:
            continue
        for code, lam_glyph, alef_glyph in ((iso, feats['init'].get(lam, lam), feats['fina'].get(cmap[alef], cmap[alef])),
                                            (fin, feats['medi'].get(lam, lam), feats['fina'].get(cmap[alef], cmap[alef]))):
            if code in cmap:
                continue
            glyph = (ligs.get((lam_glyph, alef_glyph)) or ligs.get((lam, cmap[alef])) or
                     ligs.get((lam_glyph, cmap[alef])))
            if glyph:
                new[code] = glyph
                added += 1
            else:
                missing += 1
    for table in font['cmap'].tables:
        if table.isUnicode():
            table.cmap.update(new)
    if a.family:
        for record in font['name'].names:
            if record.nameID in (1, 4, 16):
                record.string = a.family
            elif record.nameID == 6:
                record.string = a.family.replace(' ', '') + '-Regular'
    font.save(a.output)
    print(f'{a.output}: {added} presentation forms added, {missing} not found')
    return 0


if __name__ == '__main__':
    sys.exit(main())
