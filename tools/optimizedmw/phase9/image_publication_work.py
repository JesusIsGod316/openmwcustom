"""Strict completed image-publication lifetime receipts, not route/RAM savings.

The default-off ordinary control deliberately emits no shutdown receipt. A
missing candidate receipt must remain unavailable; it is never zero work or
positive evidence. Native decoder counters include startup and loading work.
"""
from pathlib import Path
import re


FINAL_PREFIX = 'OptimizedMW canonical image publication:'
POLICY_PREFIX = 'OptimizedMW canonical image publication policy:'
PRODUCER_SCOPE = 'completed_decoder_losers_not_resident_RAM_savings'
POLICY_SCOPE = 'completed_immutable_exact_source_publication_only'
SCOPE = 'game_lifetime_completed_decoder_losers'
COUNTERS = ('publications', 'winners', 'discarded_images',
            'discarded_payload_bytes_cumulative', 'revision_declines',
            'epoch_declines', 'ineligible', 'lookup_revision_misses')
UINT64_MAX = (1 << 64) - 1
SUPPLEMENTAL_PATTERN = re.compile(
    r'enabled=([01]); concurrent decoded owners only, not a whole-process RAM ceiling')


def _one_receipt(lines, prefix, label):
    matches = [(number, line) for number, line in enumerate(lines, 1) if prefix in line]
    if len(matches) != 1:
        raise ValueError('Missing or ambiguous image-publication ' + label + ' receipt')
    number, line = matches[0]
    if line.count(prefix) != 1:
        raise ValueError('Duplicate image-publication ' + label + ' receipt on one line')
    text = line.split(prefix, 1)[1].strip()
    fields = {}
    for token in text.split():
        if token.count('=') != 1:
            raise ValueError('Malformed image-publication ' + label + ' token')
        key, value = token.split('=', 1)
        if not key or not value or key in fields:
            raise ValueError('Duplicate or empty image-publication ' + label + ' field')
        fields[key] = value
    return number, fields


def _integer(token, field):
    # Do not guess unprefixed hexadecimal formatting or accept signed/float
    # counters. No independent row count constrains these lifetime integers.
    if len(token) > 20 or not re.fullmatch(r'[0-9]+', token):
        raise ValueError('Non-decimal image-publication counter: ' + field)
    value = int(token)
    if value > UINT64_MAX:
        raise ValueError('Overflow image-publication counter: ' + field)
    return value


def _validate_counts(counts, ordinary_enabled):
    if set(counts) != set(COUNTERS) or any(type(counts[key]) is not int
                                          or not 0 <= counts[key] <= UINT64_MAX for key in COUNTERS):
        raise ValueError('Invalid image-publication counter set')
    winners = counts['winners']
    images = counts['discarded_images']
    payload = counts['discarded_payload_bytes_cumulative']
    if winners < images:
        raise ValueError('Image-publication discarded images exceed accepted winners')
    if (images == 0) != (payload == 0) or payload < images:
        raise ValueError('Image-publication discarded image/payload counters disagree')
    if winners and not counts['publications']:
        raise ValueError('Image-publication reuse without a completed publication')
    if counts['ineligible'] > counts['publications']:
        raise ValueError('Image-publication fallback count exceeds ordinary publications')
    if not ordinary_enabled and any(counts.values()):
        raise ValueError('Image-publication counters engaged outside the ordinary candidate')


def lifetime_work(profile):
    """Read one complete startup/shutdown pair from the owning profile log.

    This qualifies receipt syntax and internal counters only. Normal process
    exit, transport, configuration identity and allocation-error qualification
    remain independently owned by the benchmark runner.
    """
    path = Path(profile) / 'openmw.log'
    lines = path.read_text(encoding='utf-8-sig').splitlines()
    startup_line, policy = _one_receipt(lines, POLICY_PREFIX, 'startup')
    supplemental = []
    final_lines = []
    for number, line in enumerate(lines, 1):
        if FINAL_PREFIX in line:
            if line.count(FINAL_PREFIX) != 1:
                raise ValueError('Duplicate image-publication receipt on one line')
            text = line.split(FINAL_PREFIX, 1)[1].strip()
            match = SUPPLEMENTAL_PATTERN.fullmatch(text)
            if match:
                supplemental.append((number, match[1]))
                # Preserve original line numbers. Only the exact compiled
                # engine startup format is excluded; unknown/malformed records
                # still participate in strict shutdown matching and rejection.
                final_lines.append('')
                continue
        final_lines.append(line)
    if len(supplemental) > 1:
        raise ValueError('Ambiguous image-publication supplemental startup receipt')
    final_line, final = _one_receipt(final_lines, FINAL_PREFIX, 'shutdown')
    if set(policy) != {'enabled', 'scope'} or policy['scope'] != POLICY_SCOPE or policy['enabled'] not in ('0', '1'):
        raise ValueError('Unknown or malformed image-publication startup policy')
    if set(final) != {'ordinary_enabled', 'scope', *COUNTERS} or final['scope'] != PRODUCER_SCOPE:
        raise ValueError('Unknown or missing image-publication shutdown fields/scope')
    if final['ordinary_enabled'] not in ('0', '1'):
        raise ValueError('Invalid image-publication ordinary-enabled flag')
    if startup_line >= final_line or policy['enabled'] != '1':
        raise ValueError('Image-publication shutdown disagrees with its startup policy/order')
    if supplemental and (supplemental[0][1] != policy['enabled'] or supplemental[0][0] >= final_line):
        raise ValueError('Image-publication supplemental startup disagrees with policy/order')
    counts = {key: _integer(final[key], key) for key in COUNTERS}
    enabled = final['ordinary_enabled'] == '1'
    _validate_counts(counts, enabled)
    return {
        'schema': 1, 'scope': SCOPE, 'producer_scope': PRODUCER_SCOPE,
        'enabled': enabled, 'startup_enabled': True,
        'complete_final_receipt': True,
        'counter_columns': list(COUNTERS), 'counts': counts,
        'receipt': {'log': str(path), 'startup_line': startup_line, 'shutdown_line': final_line,
                    'supplemental_startup_line': supplemental[0][0] if supplemental else None,
                    'supplemental_startup_enabled': supplemental[0][1] == '1' if supplemental else None,
                    'counter_encoding': 'decimal_uint64', 'unique_startup_and_shutdown': True},
        'limit': 'Counters cover the complete game lifetime, including initialization/loading. '
                 'Discarded payload bytes are cumulative semantic decoded losers, not decoder peaks '
                 'avoided, resident RAM/VRAM savings, measured-route work or saved milliseconds. '
                 'A syntactically complete receipt alone does not qualify normal exit, configuration '
                 'identity, allocation errors, transport or runtime performance.'
    }


def positive_engagement(work, *, requested):
    """Positive lifetime engagement only for a true requested ordinary candidate.

    Errors, missing receipts, legacy work or startup-only counters cannot be
    promoted by this helper. Call lifetime_work before passing its result.
    """
    if requested is not True or not isinstance(work, dict):
        return False
    if (work.get('schema') != 1 or work.get('scope') != SCOPE or work.get('producer_scope') != PRODUCER_SCOPE
            or work.get('enabled') is not True or work.get('startup_enabled') is not True
            or work.get('complete_final_receipt') is not True):
        return False
    counts = work.get('counts')
    if not isinstance(counts, dict):
        return False
    try:
        _validate_counts(counts, True)
    except (ValueError, KeyError, TypeError):
        return False
    return counts['discarded_images'] > 0 and counts['discarded_payload_bytes_cumulative'] > 0
