*** Settings ***
Documentation    Current local DPA binary conformance and archived route evidence.
...              No remote mutations; archive replay is not a fresh topology run.
Library    Process
Library    OperatingSystem

*** Variables ***
${PROJECT_ROOT}    ${CURDIR}/../..
${BUILD_DIR}    ${PROJECT_ROOT}/build
${PYTHON}    python3
${TOPOLOGY_ARCHIVE}    ${BUILD_DIR}/qemu-shared-b3804e0-accepted

*** Keywords ***
Run bounded conformance binary
    [Arguments]    ${name}    ${marker}
    ${binary}    Normalize Path    ${BUILD_DIR}/danos-test/${name}
    File Should Exist    ${binary}
    ${result}    Run Process    ${binary}    timeout=30s    on_timeout=terminate
    Log    ${result.stdout}
    Log    ${result.stderr}
    Should Be Equal As Integers    ${result.rc}    0
    Should Contain    ${result.stdout}    ${marker}

*** Test Cases ***
Read-only dataplane capability and ledger summary
    [Tags]    local-in-process-cli
    ${binary}    Normalize Path    ${BUILD_DIR}/danos-mgmt/tests/cli_test
    File Should Exist    ${binary}
    ${result}    Run Process    ${binary}    timeout=30s    on_timeout=terminate
    Log    ${result.stdout}
    Log    ${result.stderr}
    Should Be Equal As Integers    ${result.rc}    0
    Should Contain    ${result.stdout}    [PASS] test_dataplane_readonly

Capability and transaction conformance
    [Tags]    local-mock-contract
    Run bounded conformance binary    conformance_test    0 failed

Programming dependency delete retry and replay
    [Tags]    local-mock-contract
    Run bounded conformance binary    programming_pipeline_test    ALL PASSED

Route next-hop and group composite lifecycle
    [Tags]    local-mock-contract
    Run bounded conformance binary    route_composite_test    ALL PASSED

Archived learned route add withdraw restore and post-FRR recovery
    [Tags]    archived-evidence-only
    ${validator}    Normalize Path    ${PROJECT_ROOT}/danos-test/qemu/run_learned_route_cycle.py
    Directory Should Exist    ${TOPOLOGY_ARCHIVE}
    ${result}    Run Process    ${PYTHON}    ${validator}
    ...    --topology    ${TOPOLOGY_ARCHIVE}    timeout=60s    on_timeout=terminate
    Log    ${result.stdout}
    Log    ${result.stderr}
    Should Be Equal As Integers    ${result.rc}    0
    Should Contain    ${result.stdout}    [PASS] VPP-originated learned-route lifecycle
