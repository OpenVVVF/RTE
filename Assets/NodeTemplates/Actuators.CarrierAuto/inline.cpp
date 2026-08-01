/* Piecewise-linear carrier schedule: flat below the first and above the last
 * valid breakpoint, linear between.  A breakpoint with fsw <= 0 ends the
 * table.  The platform call deadbands small changes, so this can run every
 * app_loop step without churning the timer. */
const float fe = platform_get_elec_freq_hz();
const float fes[6]  = {fe_1, fe_2, fe_3, fe_4, fe_5, fe_6};
const float fsws[6] = {fsw_1, fsw_2, fsw_3, fsw_4, fsw_5, fsw_6};

int n_pts = 0;
while (n_pts < 6 && fsws[n_pts] > 0.0f) ++n_pts;

if (n_pts > 0) {
    float carrier = fsws[0];
    if (fe > fes[0]) {
        carrier = fsws[n_pts - 1];
        for (int i = 1; i < n_pts; ++i) {
            if (fe <= fes[i]) {
                const float span = fes[i] - fes[i - 1];
                const float t = (span > 0.0f) ? (fe - fes[i - 1]) / span : 1.0f;
                carrier = fsws[i - 1] + t * (fsws[i] - fsws[i - 1]);
                break;
            }
        }
    }
    platform_pwm_set_carrier_hz(carrier);
}
