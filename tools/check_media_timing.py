# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms and permissions: ADDITIONAL-TERMS.md
"""Inspect complete-core reports; never equate constrained STA with HW acceptance."""
import json
from pathlib import Path
import re

from toolpaths import ROOT


def inspect():
    folder=ROOT/'build/pocket-media-core/src/fpga/targets/pocket'
    summary=(folder/'output_files/ap_core.sta.summary').read_text(encoding='utf-8')
    checks=[float(x) for x in re.findall(r'^Slack : ([-.0-9]+)',summary,re.M)]
    if not checks or any(v<0 for v in checks):raise ValueError('Missing/failing constrained timing summary')
    groups={}
    for line in (folder/'media-gray-delays.rpt').read_text(encoding='utf-8').splitlines():
        fields=[s.strip() for s in line.split(';')]
        if len(fields)<8 or fields[1]!='--':continue
        match=re.search(r'(input_queue|output_queue)\|(wg_r1|rg_w1)\[(\d+)\]',fields[6])
        if not match:continue
        key=match[1]+'/'+match[2]
        group=groups.setdefault(key,{'bits':set(),'minimum_slack_ns':99.,'maximum_net_delay_ns':0.})
        group['bits'].add(int(match[3]))
        group['minimum_slack_ns']=min(group['minimum_slack_ns'],float(fields[2]))
        group['maximum_net_delay_ns']=max(group['maximum_net_delay_ns'],float(fields[4]))
    required={q+'/'+r for q in ('input_queue','output_queue') for r in ('wg_r1','rg_w1')}
    if set(groups)!=required:raise ValueError('Gray crossing group missing')
    for group in groups.values():
        # Duplicating one lower bit cannot compensate for an absent MSB.
        if group['bits']!=set(range(9)) or group['minimum_slack_ns']<0 or group['maximum_net_delay_ns']>8:
            raise ValueError('Gray crossing bit coverage/delay failed')
        group['bits']=sorted(group['bits'])
    fit=(folder/'output_files/ap_core.fit.rpt').read_text(encoding='utf-8',errors='replace')
    if 'Ignored assignment ANALYZE_METASTABILITY' in fit or 'contains an invalid node name' in fit:
        raise ValueError('Fresh fitter still rejects synchronizer assignments')
    fit_summary=(folder/'output_files/ap_core.fit.summary').read_text(encoding='utf-8')
    if 'Fitter Status : Successful' not in fit_summary:
        raise ValueError('Full fitter result is missing/failing')
    meta=(folder/'media-metastability.rpt').read_text(encoding='utf-8')
    summary_meta=meta.split('Synchronizer Chain #1:')[0]
    meta_groups={}
    status=set()
    for line in summary_meta.splitlines():
        fields=[s.strip() for s in line.split(';')]
        if len(fields)<4 or 'pocket_jpeg_mmio:jpeg|' not in fields[2]:continue
        match=re.search(r'(input_queue|output_queue)\|(wg_r1|rg_w1)\[(\d+)\]',fields[2])
        if match:meta_groups.setdefault(match[1]+'/'+match[2],set()).add(int(match[3]))
        match=re.search(r'\|(done_sync|fault_sync|idle_sync)\[0\]',fields[2])
        if match:status.add(match[1])
    if set(meta_groups)!=required or any(bits!=set(range(9)) for bits in meta_groups.values()):
        raise ValueError('Metastability report omits a JPEG pointer synchronizer')
    if status!={'done_sync','fault_sync','idle_sync'}:
        raise ValueError('Metastability report omits a JPEG status synchronizer')
    chains=re.search(r'Number of Synchronizer Chains Found: (\d+)',meta)
    unavailable=re.search(r'Fraction of Chains for which MTBFs Could Not be Calculated: ([.\d]+)',meta)
    report={'constrained_summary_checks':len(checks),'minimum_constrained_slack_ns':min(checks),
        'gray_groups':groups,'gray_report_scope':'default routed timing corner',
        'fresh_fit_successful':True,'rejected_synchronizer_assignments':False,
        'jpeg_pointer_synchronizers_reported':36,'jpeg_status_synchronizers_reported':sorted(status),
        'reported_synchronizer_chains':int(chains[1]) if chains else None,
        'fraction_mtbf_not_calculated':float(unavailable[1]) if unavailable else None,
        'physical_acceptance':False,'unconstrained_io_review_required':True,
        'board_cdc_review_required':True}
    (folder/'media-timing-audit.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(f'PASS: fresh fit, {len(checks)} nonnegative timing checks, all 36 Gray bits <=8ns, JPEG pointer/status chains reported. Board IO/CDC and hardware review remain.')
    return report


if __name__=='__main__':inspect()
