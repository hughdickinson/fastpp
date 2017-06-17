#include "fast++.hpp"

extern "C" {
    #include "tinyexpr.h"
}

double expr_step(double a) {
    return (a >= 0 ? 1.0 : 0.0);
}

double expr_min(double a, double b) {
    return std::min(a, b);
}

double expr_max(double a, double b) {
    return std::max(a, b);
}

bool gridder_t::tinyexpr_wrapper::compile(const gridder_t& gridder) {
    uint_t nparam = gridder.opts.custom_params.size()+1;
    uint_t nfunc = 3;

    vars_glue = new te_variable[nparam+nfunc];
    for (uint_t p : range(nparam+nfunc)) {
        vars_glue[p].context = nullptr;
    }

    vars.resize(nparam);

    // Time
    vars_glue[0].name = "t";
    vars_glue[0].address = &vars[0];
    vars_glue[0].type = TE_VARIABLE;

    // Custom parameters
    for (uint_t p : range(1, nparam)) {
        vars_glue[p].name = gridder.opts.custom_params[p-1].c_str();
        vars_glue[p].address = &vars[p];
        vars_glue[p].type = TE_VARIABLE;
    }

    // Custom functions
    vars_glue[nparam+0].name = "step";
    vars_glue[nparam+0].address = (void*)(&expr_step);
    vars_glue[nparam+0].type = TE_FUNCTION1;
    vars_glue[nparam+1].name = "min";
    vars_glue[nparam+1].address = (void*)(&expr_min);
    vars_glue[nparam+1].type = TE_FUNCTION2;
    vars_glue[nparam+2].name = "max";
    vars_glue[nparam+2].address = (void*)(&expr_max);
    vars_glue[nparam+2].type = TE_FUNCTION2;

    // Compile expression
    int err = 0;
    expr = te_compile(gridder.opts.custom_sfh.c_str(), vars_glue, nparam+nfunc, &err);
    if (err > 0) {
        std::string head = "could not parse SFH expression: ";
        error(head, gridder.opts.custom_sfh);
        error(std::string(head.size()+err-1, ' ')+'^');
        return false;
    }

    return true;
}

double gridder_t::tinyexpr_wrapper::eval() {
    return te_eval(expr);
}

gridder_t::tinyexpr_wrapper::~tinyexpr_wrapper() {
    if (expr != nullptr) {
        te_free(expr);
        delete[] vars_glue;
    }
}

struct ssp_bc03 {
    vec1d age;
    vec1d mass;
    vec1d lambda;
    vec2d sed;

    bool read_ascii(std::string filename) {
        std::string state = "";

        try {
            std::ifstream in(filename);
            in.exceptions(in.failbit);

            state = "read number of time steps";
            uint_t ntime = 0;
            in >> ntime;

            state = "read time steps";
            age.resize(ntime);
            for (uint_t i : range(age)) {
                in >> age[i];
            }

            state = "read IMF and other parameters";
            double ml, mu;
            uint_t iseg;
            in >> ml >> mu >> iseg;
            for (uint_t i = 0; i < iseg; ++i) {
                double xx, lm, um, baux, cn, cc;
                in >> xx >> lm >> um >> baux >> cn >> cc;
            }

            state = "read additional parameters";
            double totm, totn, avs, jo, tau, id, tau1, tau2, tau3, tau4;
            in >> totm >> totn >> avs >> jo >> tau >> id >> tau1 >> tau2 >> tau3 >> tau4;

            char id2;
            in >> id2;
            in.ignore(std::numeric_limits<std::streamsize>::max(), '\n');

            std::string id3, iop, stelib;
            std::getline(in, id3);
            std::getline(in, iop);
            std::getline(in, stelib);

            state = "read number of wavelength elements";
            uint_t nlam;
            in >> nlam;

            state = "read wavelength elements";
            lambda.resize(nlam);
            for (uint_t i : range(lambda)) {
                in >> lambda[i];
            }

            state = "read SEDs";
            sed.resize(ntime, nlam);
            for (uint_t it : range(ntime)) {
                uint_t nstep = 0;
                in >> nstep;

                if (nstep != nlam) {
                    error("corrupted file, wavelength step mismatch: ", nstep, " vs. ", nlam);
                    error("reading time step ", it, " of ", ntime);
                    return 1;
                }

                for (uint_t il : range(lambda)) {
                    in >> sed(it,il);
                }

                // Read extra information
                uint_t nfunc = 0;
                in >> nfunc;

                for (uint_t i = 0; i < nfunc; ++i) {
                    double f;
                    in >> f;
                }
            }

            state = "read extras";
            uint_t nextra = 12;
            for (uint_t ie : range(nextra)) {
                uint_t nstep = 0;
                in >> nstep;

                vec1d extra(nstep);
                for (uint_t i : range(nstep)) {
                    in >> extra[i];
                }

                if (ie == 1) {
                    mass = extra/totm;
                }
            }
        } catch (...) {
            print("");
            error("could not read data in library file '", filename, "'");
            error("could not ", state);
            error("the file is probably corrupted, try re-downloading it");
            print("");
            return false;
        }

        // Write FITS file for faster reading next time
        fits::write_table(filename+".fits", ftable(age, mass, lambda, sed));

        return true;
    }

    bool read_fits(std::string filename, bool noflux) {
        fits::input_table itbl(filename);
        if (!itbl.read_column("age", age)) {
            print("");
            error("could not read column 'T' (time) from FITS file '", filename, "'");
            print("");
            return false;
        }
        if (!itbl.read_column("mass", mass)) {
            print("");
            error("could not read column 'MASS' from FITS file '", filename, "'");
            print("");
            return false;
        }
        if (!noflux) {
            if (!itbl.read_column("lambda", lambda)) {
                print("");
                error("could not read column 'LAMBDA' from FITS file '", filename, "'");
                print("");
                return false;
            }
            if (!itbl.read_column("sed", sed)) {
                print("");
                error("could not read column 'SED' from FITS file '", filename, "'");
                print("");
                return false;
            }
        }
        return true;
    }

    bool read(std::string filename, bool noflux = false) {
        if (file::exists(filename+".ised_ASCII.fits")) {
            return read_fits(filename+".ised_ASCII.fits", noflux);
        } else if (file::exists(filename+".ised_ASCII")) {
            return read_ascii(filename+".ised_ASCII");
        } else {
            print("");
            error("could not find library: '", filename, "'");
            error("expected extensions *.fits or *.ised_ASCII");
            print("");
            return false;
        }
    }
};

std::string gridder_t::get_library_file_ssp(uint_t im) const {
    return opts.library_dir+"ssp"+"."+opts.resolution+"/"+
        opts.library+"_"+opts.resolution+"_"+opts.name_imf+
        "_z"+replace(strn(output.grid[grid_id::metal][im]), "0.", "");
}

void gridder_t::evaluate_sfh_custom(const vec1u& idm, const vec1d& t, vec1d& sfh) const {
    for (uint_t i : range(opts.custom_params)) {
        sfh_expr.vars[i+1] = output.grid[grid_id::custom+i][idm[grid_id::custom+i]];
    }

    sfh.resize(t.size());
    for (uint_t i : range(t)) {
        sfh_expr.vars[0] = t.safe[i];
        sfh.safe[i] = sfh_expr.eval();
    }
}

bool gridder_t::build_and_send_custom(fitter_t& fitter) {
    model_t model;
    model.flux.resize(input.lambda.size());
    model.props.resize(nprop);

    ssp_bc03 ssp;

    const vec1f& output_metal = output.grid[grid_id::metal];
    const vec1f& output_age = output.grid[grid_id::age];
    const vec1f& output_z = output.grid[grid_id::z];

    float& model_mass = model.props[prop_id::mass];
    float& model_sfr = model.props[prop_id::sfr];
    float& model_ssfr = model.props[prop_id::ssfr];

    double dt = opts.custom_sfh_step;
    vec1d ctime = reverse(dt*dindgen(uint_t(ceil(e10(max(output_age))/dt)+1.0)));
    // NB: age array is sorted from largest to smallest

    auto pg = progress_start(nmodel);
    for (uint_t im : range(output_metal)) {
        vec1u idm(nparam);
        idm[grid_id::metal] = im;

        // Load SSP
        std::string filename = get_library_file_ssp(im);
        if (!ssp.read(filename)) {
            return false;
        }

        // Pre-compute dust law & IGM absorption (they don't change with age)
        vec1d dust_law = build_dust_law(ssp.lambda);
        vec2d igm_abs = build_igm_absorption(output_z, ssp.lambda);

        // Build SSP delta_t array
        vec1d ssp_t1(ssp.age.size());
        vec1d ssp_t2(ssp.age.size());
        for (uint_t it : range(ssp.age)) {
            if (it == 0) {
                ssp_t1.safe[it] = 0;
            } else {
                ssp_t1.safe[it] = ssp_t2.safe[it-1];
            }
            if (it == ssp.age.size()-1) {
                ssp_t2.safe[it] = ssp.age.back();
            } else {
                ssp_t2.safe[it] = 0.5*(ssp.age.safe[it] + ssp.age.safe[it+1]);
            }
        }

        for (uint_t ic = 0; ic < ncustom; ++ic) {
            // Build analytic SFH
            vec1d sfh;
            evaluate_sfh_custom(idm, ctime, sfh);

            for (uint_t ia : range(output_age)) {
                idm[grid_id::age] = ia;

                // Integrate SFH on local time grid
                vec1d tpl_flux(ssp.lambda.size());
                double tmodel_mass = 0.0;

                double formed = 0;
                vec1d ltime = e10(output_age[ia]) - ctime;
                uint_t ihint = npos;
                for (uint_t it : range(ssp.age)) {
                    if (ssp_t1.safe[it] > ltime.back()) {
                        break;
                    }

                    double t2 = min(ssp_t2.safe[it], ltime.back());
                    formed = integrate_hinted(ltime, sfh, ihint, ssp_t1.safe[it], t2);

                    tpl_flux += formed*ssp.sed.safe(it,_);
                    tmodel_mass += formed*ssp.mass.safe[it];
                }

                model_mass = tmodel_mass;

                if (opts.sfr_avg > 0) {
                    // Average SFR over the past X yr
                    double t1 = min(opts.sfr_avg, ltime.back());
                    model_sfr = integrate(ltime, sfh, 0.0, t1)/opts.sfr_avg;
                } else {
                    // Use instantaneous SFR
                    model_sfr = formed/(ssp_t2[0] - ssp_t1[0]);
                }

                model_ssfr = model_sfr/model_mass;

                // The rest is not specific to the SFH, use generic code
                build_and_send_impl(fitter, pg, ssp.lambda, tpl_flux, dust_law, igm_abs,
                    output_age[ia], idm, model);
            }

            // Go to next model
            increment_index_list(idm, grid_dims);
        }
    }

    return true;
}

bool gridder_t::build_template_custom(uint_t iflat, vec1f& lam, vec1f& flux) const {
    vec1u idm = grid_ids(iflat);
    uint_t ia = idm[grid_id::age];
    uint_t im = idm[grid_id::metal];
    const vec1f& output_age = output.grid[grid_id::age];

    ssp_bc03 ssp;

    // Load SSP
    std::string filename = get_library_file_ssp(im);
    if (!ssp.read(filename)) {
        return false;
    }

    // Build analytic SFH
    double dt = opts.custom_sfh_step;
    vec1d ctime = reverse(dt*dindgen(uint_t(ceil(e10(max(output_age))/dt)+1.0)));
    // NB: age array is sorted from largest to smallest
    vec1d sfh;
    evaluate_sfh_custom(idm, ctime, sfh);

    // Integrate SFH on local time grid
    double mass = 0.0;
    vec1d tpl_flux(ssp.lambda.size());

    vec1d ltime = e10(output_age[ia]) - ctime;
    double t2 = 0.0;
    uint_t ihint = npos;
    for (uint_t it : range(ssp.age)) {
        double t1 = t2;
        if (it < ssp.age.size()-1) {
            t2 = 0.5*(ssp.age.safe[it] + ssp.age.safe[it+1]);
        } else {
            t2 = ssp.age.safe[it];
        }

        t2 = min(t2, ltime.back());

        double formed = integrate_hinted(ltime, sfh, ihint, t1, t2);
        mass += formed*ssp.mass.safe[it];
        tpl_flux += formed*ssp.sed.safe(it,_);

        if (t2 >= ltime.back()) {
            break;
        }
    }

    lam = ssp.lambda;
    flux = tpl_flux/mass;

    return true;
}

bool gridder_t::get_sfh_custom(uint_t iflat, const vec1d& t, vec1d& sfh,
    const std::string& type) const {

    vec1u idm = grid_ids(iflat);
    double nage = e10(output.grid[grid_id::age][idm[grid_id::age]]);
    double age_obs = e10(auniv[grid_id::z]);
    double age_born = age_obs - nage;

    // Evaluate SFH
    uint_t i0 = upper_bound(age_born, t);
    uint_t i1 = upper_bound(age_obs, t);

    if (i1 == npos) {
        i1 = t.size()-1;
    }
    if (i0 == npos) {
        i0 = 0;
    }

    sfh = replicate(0.0, t.size()); {
        vec1d tsfh;
        evaluate_sfh_custom(idm, t[i0-_] - age_born, tsfh);
        sfh[i0-_] = tsfh;
    }

    // Load SSP (only extras) to get mass
    std::string filename = get_library_file_ssp(idm[grid_id::metal]);

    ssp_bc03 ssp;
    if (!ssp.read(filename, true)) {
        return false;
    }

    if (type == "sfr") {
        // Compute total mass at epoch of observation
        double mass = 0.0;
        vec1d ltime = nage - reverse(t);
        vec1d lsfh = reverse(sfh);

        double t2 = 0.0;
        uint_t ihint = npos;
        for (uint_t it : range(ssp.age)) {
            double t1 = t2;
            if (it < ssp.age.size()-1) {
                t2 = 0.5*(ssp.age.safe[it] + ssp.age.safe[it+1]);
            } else {
                t2 = ssp.age.safe[it];
            }

            t2 = min(t2, ltime.back());

            mass += ssp.mass.safe[it]*integrate_hinted(ltime, lsfh, ihint, t1, t2);

            if (t2 >= ltime.back()) {
                break;
            }
        }

        // Normalize to unit mass at observation
        sfh /= mass;
    } else if (type == "mass") {
        // Integrate mass, including mass loss
        vec1d mass(t.size());
        vec1d lsfh = reverse(sfh);

        for (uint_t i : range(t)) {
            double t2 = 0.0;
            vec1d ltime = t.safe[i] - reverse(t);
            uint_t ihint = npos;
            for (uint_t it : range(ssp.age)) {
                double t1 = t2;
                if (it < ssp.age.size()-1) {
                    t2 = 0.5*(ssp.age.safe[it] + ssp.age.safe[it+1]);
                } else {
                    t2 = ssp.age.safe[it];
                }

                t2 = min(t2, ltime.back());

                mass.safe[i] += ssp.mass.safe[it]*integrate_hinted(ltime, lsfh, ihint, t1, t2);

                if (t2 >= ltime.back()) {
                    break;
                }
            }
        }

        // Normalize to unit mass at observation
        mass /= interpolate(mass, t - age_born, nage);

        // Return mass instead of SFR
        std::swap(mass, sfh);
    } else {
        error("unknown SFH type '", type, "'");
        return false;
    }

    return true;
}