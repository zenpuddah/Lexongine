"""Process-level CLI contract checks; XML inspection uses Python's independent parser."""
import pathlib
import copy
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
sys.dont_write_bytecode = True
from verify_fixture import verify_snapshot

exe, example, scratch = map(pathlib.Path, sys.argv[1:])

def run(*args, expected=0):
    result = subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True)
    if result.returncode != expected:
        try:
            cause = ET.fromstring(result.stdout).find(".//field[@name='cause']")
            detail = ET.tostring(cause, encoding='unicode') if cause is not None else result.stdout
        except ET.ParseError:
            detail = result.stdout
        raise AssertionError((args, result.returncode, detail, result.stderr))
    return result

def field(record, name):
    matches = record.findall(f"./field[@name='{name}']")
    assert len(matches) == 1, (name, ET.tostring(record))
    return list(matches[0])[0]

def records(value):
    return [list(item)[0] for item in value.findall('item')]

def scalar(record, name):
    return field(record, name).text or ''

with tempfile.TemporaryDirectory(prefix='lexongine-cli.', dir=scratch) as directory:
    root = pathlib.Path(directory)
    listing = ET.fromstring(run('list').stdout)
    assert listing.tag == 'registry'
    operations = records(field(listing.find('record'), 'operations'))
    assert len(operations) >= 5
    description = ET.fromstring(run('describe', '--operation', 'statistics.sma_price', '--version', '1', '--implementation', 'builtin-v1').stdout)
    operation = records(field(description.find('record'), 'operations'))[0]
    params = records(field(field(operation, 'parameters'), 'fields'))
    assert {scalar(p, 'name'): scalar(p, 'kind') for p in params}['window'] == 'unsigned64'
    run('describe', '--operation', 'unknown.operation', '--version', '1', '--implementation', 'builtin-v1', expected=2)
    run('run', '--config', 'missing', expected=2)
    run('validate', '--config', example.with_name('cli-computation.xml'), '--reuse', 'disabled')
    unbound = root/'unbound-cli'
    shutil.copy2(exe, unbound)
    prerequisite = subprocess.run([str(unbound), 'list'], capture_output=True, text=True)
    assert prerequisite.returncode == 4, prerequisite.stderr

    # Explicit CLI fixture limits. The original core fixture remains unchanged.
    config = ET.parse(example)
    config.find('limits').set('managed_buffer_bytes', str(536870912))
    config.find('run').set('retention_log_max_events', '16384')
    config.find('run').set('retention_log_max_bytes', '2097152')
    config.find('run').set('retention_log_path', str(root / 'unused.log'))
    config.find('cache').set('directory', str(root / 'cache'))
    config.find('cache').set('max_entry_bytes', '2097152')
    path = root / 'fixture.xml'
    config.write(path, encoding='unicode')
    run('validate', '--config', path)
    assert not (root / 'unused.log').exists(), 'validation executed evidence callbacks'
    assert not list((root / 'cache').glob('*.xml')), 'validation published artifacts'
    plan, before, after = (root / name for name in ('plan.xml', 'before.dot', 'after.dot'))
    run('plan', '--config', path, '--plan', plan, '--before-dot', before, '--after-dot', after)
    exported = ET.parse(plan).getroot()
    assert len(exported.findall('./logical_nodes/list/item')) == 6
    assert len(exported.findall('./physical_nodes/list/item')) == 5
    assert 'alias=fast_B' in after.read_text()
    assert 'fast_A_trial' in before.read_text() and 'fast_B_trial' in after.read_text()
    first_root = ET.fromstring(run('run', '--config', path, '--report', root/'first.xml', '--retention-log', root/'first.log').stdout)
    first = first_root.find('record')
    oracle_first = verify_snapshot(root/'first.xml', first_root, expected_parameters={'fast_A': {'window': 20}, 'slow': {'window': 50}})
    assert oracle_first['requests']['fast_A_trial'][0] == (49, 100395000000)
    assert oracle_first['requests']['fast_A_trial'][-1] == (119, 101095000000)
    assert oracle_first['requests']['slow_trial'][0] == (49, 100245000000)
    assert oracle_first['requests']['slow_trial'][-1] == (119, 100945000000)
    assert all(raw == 150000000 for _, raw in oracle_first['requests']['spread_trial'])
    assert scalar(first, 'acceptance') == 'accepted' and scalar(first, 'complete_updates') == '120'
    requests = records(field(first, 'requests'))
    assert [scalar(r, 'delivered') for r in requests] == ['71'] * 4
    snapshot = ET.parse(root/'first.xml').getroot()
    assert snapshot.attrib['acceptance'] == 'pending'
    print('CLI_FIXTURE snapshot_bytes=', (root/'first.xml').stat().st_size,
          'retention_bytes=', (root/'first.log').stat().st_size,
          'published_entries=', scalar(first, 'published_entries'))
    second_root = ET.fromstring(run('run', '--config', path, '--report', root/'second.xml', '--retention-log', root/'second.log').stdout)
    second = second_root.find('record')
    oracle_cached = verify_snapshot(root/'second.xml', second_root)
    assert oracle_cached['outcomes'] == oracle_first['outcomes']
    producers = {scalar(p, 'id'): p for p in records(field(second, 'producers'))}
    assert scalar(producers['fast_A'], 'advances') == '0'
    assert scalar(producers['fast_A'], 'cached_deliveries') == '120'
    assert scalar(producers['slow'], 'advances') == '0'
    assert scalar(second, 'cache') == 'no_new_entries'
    assert scalar(first, 'published_entries') == '4'
    marker_count = len(list((root/'cache').glob('*.accepted.xml')))
    run('run', '--config', path, '--report', root/'second.xml', '--retention-log', root/'failed.log', expected=3)
    assert len(list((root/'cache').glob('*.accepted.xml'))) == marker_count
    run('validate', '--config', path, '--set', 'fast_A.window=01', expected=2)
    run('validate', '--config', path, '--observe', 'unknown', expected=2)
    override = ET.fromstring(run('validate', '--config', path, '--set', 'fast_A.window=21', '--observe', 'fast_A_trial', '--reuse', 'disabled').stdout)
    effective = override.find('./effective_config/record')
    assert len(records(field(effective, 'observations'))) == 1
    assert any(scalar(v, 'target') == 'parameter:fast_A.window' and scalar(v, 'value') == '21' for v in records(field(effective, 'overrides')))
    baseline_nodes = records(exported.find('./logical_nodes/list'))
    changed_nodes = records(override.find('./logical_nodes/list'))
    assert scalar(changed_nodes[2], 'canonical_identity') != scalar(baseline_nodes[2], 'canonical_identity')
    unshared = ET.fromstring(run('validate', '--config', path, '--sharing', 'disabled', '--reuse', 'disabled').stdout)
    assert len(unshared.findall('./physical_nodes/list/item')) == 6
    unshared_root = ET.fromstring(run('run','--config',path,'--report',root/'unshared.xml','--retention-log',root/'unshared.log','--sharing','disabled','--reuse','disabled').stdout)
    unshared_run = unshared_root.find('record')
    oracle_unshared = verify_snapshot(root/'unshared.xml', unshared_root)
    assert oracle_unshared['outcomes'] == oracle_first['outcomes']
    assert [scalar(r, 'delivered') for r in records(field(unshared_run, 'requests'))] == ['71'] * 4
    unshared_producers = {scalar(p, 'id'): p for p in records(field(unshared_run, 'producers'))}
    assert scalar(unshared_producers['fast_B'], 'advances') == '120'
    lag_config = copy.deepcopy(config)
    lag = ET.Element('node', id='lagged_slow', operation='series.lag_price', semantic_version='1', implementation_id='builtin-v1', alignment='same_step_activation', initialization='empty_at_step_0')
    ET.SubElement(lag, 'param', name='lag_steps', value='1')
    ET.SubElement(lag, 'param', name='count_basis', value='complete_bar_updates')
    ET.SubElement(lag, 'input', port='price', producer='slow', output='mean', prior_outputs='0', missing_intervals='strict')
    lag_config.find('nodes').insert(4, lag)
    right = lag_config.find("./nodes/node[@id='spread']/input[@port='right']")
    right.set('producer', 'lagged_slow'); right.set('output', 'lagged')
    for observation in lag_config.findall('./observations/observe'):
        observation.set('begin_step', '50')
    lag_path = root/'lag.xml'; lag_config.write(lag_path, encoding='unicode')
    lag_results = []
    for mode, overrides in [('fresh', []), ('cached', []), ('unshared', ['--sharing','disabled','--reuse','disabled'])]:
        report = root/f'lag-{mode}.snapshot.xml'
        receipt = ET.fromstring(run('run','--config',lag_path,'--report',report,'--retention-log',root/f'lag-{mode}.log',*overrides).stdout)
        checked = verify_snapshot(report, receipt, expected_parameters={'lagged_slow': {'lag_steps': 1}})
        assert all(len(v) == 70 for v in checked['requests'].values())
        assert all(v == 160000000 for _, v in checked['requests']['spread_trial'])
        lag_results.append(checked['outcomes'])
    assert lag_results[0] == lag_results[1] == lag_results[2]
    # A rejected slow request does not erase independently admitted fast requests.
    partial = ET.fromstring(run('run', '--config', path, '--report', root/'partial.xml', '--retention-log', root/'partial.log', '--set', 'slow.window=51', '--reuse', 'disabled', expected=6).stdout).find('record')
    assert scalar(partial, 'acceptance') == 'accepted'
    assert [scalar(r, 'admitted') for r in records(field(partial, 'requests'))] == ['true','true','false','false']
    # Enforced execution/evidence failure is exit 3 and retains a structured cause.
    config.find('run').set('retention_log_max_bytes', '64')
    config.write(root/'quota.xml', encoding='unicode')
    quota = ET.fromstring(run('run', '--config', root/'quota.xml', '--report', root/'quota-report.xml', '--retention-log', root/'quota.log', '--reuse', 'disabled', expected=3).stdout).find('record')
    assert scalar(quota, 'acceptance') == 'rejected'
    assert scalar(field(quota, 'cause').find('record'), 'code') == 'EvidenceQuota'
    config.find('run').set('retention_log_max_bytes', '2097152')
    config.find('run').set('driver', 'hosted')
    config.write(root/'hosted.xml', encoding='unicode')
    run('run', '--config', root/'hosted.xml', '--report', root/'hosted-report.xml', expected=2)
    # XML/DOT escaping must preserve logical identifiers and observation labels.
    config.find('run').set('driver', 'standalone')
    escaped_id = 'fast_"&<>\\id'
    config.find("./nodes/node[@id='fast_A']").set('id', escaped_id)
    config.find("./nodes/node[@id='spread']/input[@port='left']").set('producer', escaped_id)
    config.find("./observations/observe[@id='fast_A_trial']").set('producer', escaped_id)
    config.find("./observations/observe[@id='fast_A_trial']").set('id', escaped_id+'trial')
    config.write(root/'escaped.xml', encoding='unicode')
    run('plan','--config',root/'escaped.xml','--plan',root/'escaped-plan.xml','--before-dot',root/'escaped-before.dot','--after-dot',root/'escaped-after.dot','--reuse','disabled')
    escaped_plan = ET.parse(root/'escaped-plan.xml').getroot()
    assert scalar(field(records(escaped_plan.find('./logical_nodes/list'))[2], 'spec'), 'id') == escaped_id
    assert r'fast_\"&<>\\id' in (root/'escaped-before.dot').read_text()
    # Plan files are create-new and output aliases are rejected before publication.
    run('plan','--config',path,'--plan',plan,'--before-dot',root/'x.dot','--after-dot',root/'y.dot',expected=3)
    run('plan','--config',path,'--plan',root/'alias.xml','--before-dot',root/'alias.xml','--after-dot',root/'z.dot',expected=2)
    print('PASS CLI registry, full-fixture fresh/reuse, graph/override inspection, exits 0/2/3/6, escaping and immutable outputs')
