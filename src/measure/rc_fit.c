/**
 * @file  rc_fit.c
 * @brief Voltage-domain exponential fit for the RC charge curve.
 *
 * The model is
 *
 *     V(t) = Vinf - (Vinf - V0) * exp(-(t - t0) / tau)
 *
 * and Vinf, tau and t0 are solved **jointly** by damped Gauss-Newton
 * (Levenberg-Marquardt).
 *
 * Why not the classic log-linearisation? Because it assumes Vinf is known
 * exactly. In the ESP32 firmware the asymptote came from a coarse settle loop,
 * and a ~60 mV asymptote error produced a ~-7 % tau error on a synthetic
 * model. Fitting in the voltage domain removes that sensitivity, which matters
 * here because capacitance is exactly proportional to tau.
 *
 * The reported r2 is deliberately still computed in the **log domain** against
 * the fitted asymptote: the fusion quality scoring was tuned against that
 * definition, so keeping it preserves the meaning of every quality threshold
 * carried over from the ESP32 firmware.
 *
 * This file is pure arithmetic. It must never include a HAL header, so it can
 * be compiled and tested on a host.
 */

#include "rc_fit.h"

#include <string.h>

/* Working storage. Static rather than stack: a 256-point fit needs ~4 KB,
 * which is more than is comfortable on a measurement call stack, and only one
 * fit ever runs at a time. */
static double s_xs[RC_FIT_MAX_PTS];
static double s_ys[RC_FIT_MAX_PTS];

/**
 * @brief Solve A*x = b by Gauss-Jordan elimination with partial pivoting.
 *
 * Returns false when the matrix is numerically singular. A 3x3 system is small
 * enough that no library is warranted, and keeping it here means the fit has
 * no external dependency.
 */
bool rc_solve3(double A[3][3], const double b[3], double x[3])
{
    double M[3][4];

    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            M[i][j] = A[i][j];
        }
        M[i][3] = b[i];
    }

    for (int col = 0; col < 3; ++col) {
        int piv = col;
        for (int r = col + 1; r < 3; ++r) {
            if (fabs(M[r][col]) > fabs(M[piv][col])) {
                piv = r;
            }
        }
        if (fabs(M[piv][col]) < 1e-12) {
            return false;
        }
        if (piv != col) {
            for (int j = col; j < 4; ++j) {
                double t = M[col][j];
                M[col][j] = M[piv][j];
                M[piv][j] = t;
            }
        }

        double d = M[col][col];
        for (int j = col; j < 4; ++j) {
            M[col][j] /= d;
        }

        for (int r = 0; r < 3; ++r) {
            if (r == col) {
                continue;
            }
            double f = M[r][col];
            if (f == 0.0) {
                continue;
            }
            for (int j = col; j < 4; ++j) {
                M[r][j] -= f * M[col][j];
            }
        }
    }

    for (int i = 0; i < 3; ++i) {
        x[i] = M[i][3];
    }
    return true;
}

/** Model value at time x (us). */
static inline double rc_model(double x, double v_inf, double tau, double t0, double v0)
{
    return v_inf - (v_inf - v0) * exp(-(x - t0) / tau);
}

/**
 * @brief Fit the exponential model to sampled points.
 *
 * A monotonic glitch filter runs first: any point more than RC_FIT_GLITCH_MV
 * below the running maximum is discarded. On the ESP32 this rejected ADC
 * spikes; on the G474 the acquisition is timer-paced and oversampled, so
 * genuine spikes are rarer, but a comparator edge or a mux transient can still
 * inject one. The filter is cheap and keeps a single bad point from biasing
 * the fit.
 */
rc_fit_t rc_fit_samples(const double *t_us, const double *v_mv, int n,
                        double v0_mv, double v_inf_hint)
{
    rc_fit_t r;
    memset(&r, 0, sizeof(r));

    if (t_us == NULL || v_mv == NULL || n < RC_FIT_MIN_PTS) {
        return r;
    }

    /* ---- Glitch filter --------------------------------------------------- */
    int m = 0;
    double run_max = v_mv[0];
    for (int i = 0; i < n && m < RC_FIT_MAX_PTS; ++i) {
        if (v_mv[i] > run_max) {
            run_max = v_mv[i];
        }
        if (v_mv[i] < run_max - (double)RC_FIT_GLITCH_MV) {
            continue;
        }
        s_xs[m] = t_us[i] - t_us[0];
        s_ys[m] = v_mv[i];
        ++m;
    }
    if (m < RC_FIT_MIN_PTS) {
        return r;
    }

    double y_max = s_ys[0];
    for (int i = 1; i < m; ++i) {
        if (s_ys[i] > y_max) {
            y_max = s_ys[i];
        }
    }

    /* ---- Initial parameters ---------------------------------------------- */
    double v_inf = (v_inf_hint > 0.0) ? v_inf_hint : (y_max + 200.0);
    if (v_inf < y_max + 2.0) {
        /* The asymptote must exceed every observed sample, otherwise the
         * model cannot fit the data at all. */
        v_inf = y_max + 200.0;
    }

    double t0 = 0.0;

    /* Seed tau from the model-free 63.2 % crossing of the initial guess. */
    double v63 = v0_mv + 0.632 * (v_inf - v0_mv);
    double tau = 0.0;
    for (int i = 0; i < m; ++i) {
        if (s_ys[i] >= v63) {
            tau = s_xs[i] - t0;
            break;
        }
    }
    if (tau <= 0.0) {
        tau = s_xs[m - 1] * 0.4;
    }
    if (tau <= 1e-3) {
        tau = 1.0;
    }

    /* ---- Damped Gauss-Newton -------------------------------------------- */
    double cost = 0.0;
    for (int i = 0; i < m; ++i) {
        double res = rc_model(s_xs[i], v_inf, tau, t0, v0_mv) - s_ys[i];
        cost += res * res;
    }

    /* Variance of the data about its own mean. A fit that cannot beat this is
     * not describing an exponential at all — it is just re-centring noise. The
     * check is deliberately loose (the fit only has to explain 10 % of the
     * variance); its purpose is to reject a flat or non-monotonic capture
     * outright so callers never see "ok" from a dead acquisition. */
    double y_mean = 0.0;
    for (int i = 0; i < m; ++i) {
        y_mean += s_ys[i];
    }
    y_mean /= (double)m;

    double ss_total = 0.0;
    for (int i = 0; i < m; ++i) {
        double d = s_ys[i] - y_mean;
        ss_total += d * d;
    }
    if (ss_total <= 1.0) {
        return r;   /* no variation worth fitting */
    }

    double lambda = 1e-2;
    bool converged = false;

    for (int it = 0; it < RC_FIT_MAX_ITER && !converged; ++it) {
        double amp = v_inf - v0_mv;
        if (amp <= 0.0) {
            break;
        }

        double A[3][3] = {{0.0}};
        double g[3] = {0.0};

        for (int i = 0; i < m; ++i) {
            double s = (s_xs[i] - t0) / tau;
            if (s < -20.0) {
                s = -20.0;
            }
            double e = exp(-s);
            double res = (v_inf - amp * e) - s_ys[i];

            double J[3];
            J[0] = 1.0 - e;                                        /* d/dVinf */
            J[1] = -amp * e * (s_xs[i] - t0) / (tau * tau);         /* d/dtau  */
            J[2] = -amp * e / tau;                                  /* d/dt0   */

            for (int a = 0; a < 3; ++a) {
                g[a] += J[a] * res;
                for (int b = 0; b < 3; ++b) {
                    A[a][b] += J[a] * J[b];
                }
            }
        }

        /* Levenberg-Marquardt damping, scaled by the diagonal so the three
         * parameters (which differ by orders of magnitude) are treated
         * comparably. */
        double Ad[3][3];
        for (int a = 0; a < 3; ++a) {
            for (int b = 0; b < 3; ++b) {
                Ad[a][b] = A[a][b];
            }
        }
        for (int a = 0; a < 3; ++a) {
            Ad[a][a] += lambda * (A[a][a] > 0.0 ? A[a][a] : 1.0);
        }

        double delta[3];
        if (!rc_solve3(Ad, g, delta)) {
            lambda *= 10.0;
            if (lambda > 1e12) {
                break;
            }
            continue;
        }

        double nv = v_inf - delta[0];
        double nt = tau - delta[1];
        double n0 = t0 - delta[2];

        /* Keep the parameters physical. */
        if (nt < 1e-3) {
            nt = 1e-3;
        }
        if (nv <= v0_mv) {
            nv = v0_mv + 1.0;
        }
        /* 1.2 x nominal 3.3 V: the node cannot exceed the rail by much, and an
         * unbounded Vinf lets the fit escape to absurd values on flat data. */
        if (nv > 1.2 * 3300.0) {
            nv = 1.2 * 3300.0;
        }
        /* t0 is the excitation edge, which precedes the first stored sample. */
        if (n0 > 0.0) {
            n0 = 0.0;
        }

        double trial = 0.0;
        for (int i = 0; i < m; ++i) {
            double res = rc_model(s_xs[i], nv, nt, n0, v0_mv) - s_ys[i];
            trial += res * res;
        }

        if (trial < cost) {
            double dtau = fabs(nt - tau);
            double dvinf = fabs(nv - v_inf);
            v_inf = nv;
            tau = nt;
            t0 = n0;
            cost = trial;
            lambda *= 0.5;
            if (lambda < 1e-9) {
                lambda = 1e-9;
            }
            if (dtau < 1e-6 * tau && dvinf < 1e-6 * v_inf) {
                converged = true;
            }
        } else {
            lambda *= 4.0;
            if (lambda > 1e12) {
                break;
            }
        }
    }

    if (!(tau > 0.0) || !(v_inf > v0_mv)) {
        return r;
    }

    /* Reject a fit with no explanatory power (see ss_total above). */
    if (cost >= 0.90 * ss_total) {
        return r;
    }

    /* ---- Goodness of fit ------------------------------------------------- */
    double rmse = sqrt(cost / (double)m);

    /* Log-domain R2 against the fitted asymptote. Samples within 12 mV of the
     * asymptote are excluded: log() of a near-zero gap is dominated by ADC
     * quantisation and would make R2 meaningless. */
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    int k = 0;
    for (int i = 0; i < m; ++i) {
        double gap = v_inf - s_ys[i];
        if (gap < 12.0) {
            continue;
        }
        double y = log(gap);
        sx += s_xs[i];
        sy += y;
        sxx += s_xs[i] * s_xs[i];
        sxy += s_xs[i] * y;
        ++k;
    }

    double r2 = 0.0;
    if (k >= 4) {
        double denom = (double)k * sxx - sx * sx;
        if (fabs(denom) > 1e-9) {
            double slope = ((double)k * sxy - sx * sy) / denom;
            double intercept = (sy - slope * sx) / (double)k;
            double ybar = sy / (double)k;
            double ss_res = 0.0, ss_tot = 0.0;
            for (int i = 0; i < m; ++i) {
                double gap = v_inf - s_ys[i];
                if (gap < 12.0) {
                    continue;
                }
                double y = log(gap);
                double yhat = slope * s_xs[i] + intercept;
                ss_res += (y - yhat) * (y - yhat);
                ss_tot += (y - ybar) * (y - ybar);
            }
            r2 = (ss_tot > 1e-12) ? (1.0 - ss_res / ss_tot) : 0.0;
        }
    }

    /* ---- tau standard error from the covariance sigma^2 * (J^T J)^-1 ----- */
    double tau_se = 0.0;
    if (m > 3) {
        double A[3][3] = {{0.0}};
        double amp = v_inf - v0_mv;
        for (int i = 0; i < m; ++i) {
            double s = (s_xs[i] - t0) / tau;
            if (s < -20.0) {
                s = -20.0;
            }
            double e = exp(-s);
            double J[3];
            J[0] = 1.0 - e;
            J[1] = -amp * e * (s_xs[i] - t0) / (tau * tau);
            J[2] = -amp * e / tau;
            for (int a = 0; a < 3; ++a) {
                for (int b = 0; b < 3; ++b) {
                    A[a][b] += J[a] * J[b];
                }
            }
        }
        double sigma2 = cost / (double)(m - 3);
        double unit[3] = {0.0, 1.0, 0.0};
        double col[3];
        if (rc_solve3(A, unit, col)) {
            double var = sigma2 * col[1];
            if (var > 0.0) {
                tau_se = sqrt(var);
            }
        }
    }

    r.ok = true;
    r.tau_us = tau;
    r.v_inf_mv = v_inf;
    r.t0_us = t0;
    r.r2 = r2;
    r.rmse_mv = rmse;
    r.tau_se_us = tau_se;
    r.n = m;
    return r;
}

/**
 * @brief Uniform-sampling convenience wrapper.
 *
 * On the G474 the ADC is paced by TIM6, so point i is at exactly i * dt_us.
 * No timestamps are recorded or needed, which is one of the reasons the
 * capture path is simpler than the ESP32 version.
 */
rc_fit_t rc_fit_uniform(const double *v_mv, int n, double dt_us,
                        double v0_mv, double v_inf_hint)
{
    static double t[RC_FIT_MAX_PTS];

    if (n > RC_FIT_MAX_PTS) {
        n = RC_FIT_MAX_PTS;
    }
    for (int i = 0; i < n; ++i) {
        t[i] = (double)i * dt_us;
    }
    return rc_fit_samples(t, v_mv, n, v0_mv, v_inf_hint);
}
