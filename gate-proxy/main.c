#include "app_shell.h"
#include "gate_telemetry.h"
#include "persistence.h"
#include "laser.h"
#include "net_layer.h"
#include "net_monitor.h"
#include "api/api.h"

// TODO: add radio transport security?
// TODO: program a button to learn gate code
// TOOD: gracefully degrade in abscence of laser, support code send

int main(void)
{
    laser_init();

    // restore any existing persisted state
    restore_state();

    netl_init();

    netm_start();
    api_start();
    app_shell_run();
    // TODO: set led if teaching needed

    return 0;
}