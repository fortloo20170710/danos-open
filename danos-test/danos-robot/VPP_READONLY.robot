*** Settings ***
Documentation    Local VPP read-only checks inspired by i-danos/tests layering.
...              No configuration writes; not packet/performance qualification.
Library    ${CURDIR}/vpp_readonly.py    ${VPPCTL}    ${VPP_SOCKET}
Suite Setup    Interface Identity Should Match    ${IDENTITY_INTERFACE}    ${EXPECTED_MAC}

*** Variables ***
${VPPCTL}    /usr/bin/vppctl
${VPP_SOCKET}    /run/vpp/cli.sock
${IDENTITY_INTERFACE}    GigabitEthernet1/0/0
${EXPECTED_MAC}    MUST_BE_SET

*** Test Cases ***
Runtime version exists
    Runtime Version Should Be Available

Physical counters exist
    Interface Counters Should Be Available

Physical lab ECMP route is resolved
    [Tags]    r3-only
    Ecmp Route Should Have Two Buckets
