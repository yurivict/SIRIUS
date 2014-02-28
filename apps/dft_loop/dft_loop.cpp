#include <sirius.h>

int main(int argn, char** argv)
{
    using namespace sirius;
    
    Platform::initialize(1);
    
    {
        Global parameters;

        JSON_tree parser("sirius.json");

        parameters.read_unit_cell_input();

        parameters.set_lmax_apw(parser["lmax_apw"].get(10));
        parameters.set_lmax_pot(parser["lmax_pot"].get(10));
        parameters.set_lmax_rho(parser["lmax_rho"].get(10));
        parameters.set_pw_cutoff(parser["pw_cutoff"].get(20.0));
        parameters.set_aw_cutoff(parser["aw_cutoff"].get(7.0));
        parameters.set_gk_cutoff(parser["gk_cutoff"].get(7.0));
        
        parameters.unit_cell()->set_auto_rmt(parser["auto_rmt"].get(0));
        int num_mag_dims = parser["num_mag_dims"].get(0);
        int num_spins = (num_mag_dims == 0) ? 1 : 2;
        
        parameters.set_num_mag_dims(num_mag_dims);
        parameters.set_num_spins(num_spins);

        parameters.initialize();
        
        Potential* potential = new Potential(parameters);
        potential->allocate();

        std::vector<int> ngridk = parser["ngridk"].get(std::vector<int>(3, 1));
            
        int numkp = ngridk[0] * ngridk[1] * ngridk[2];
        int ik = 0;
        mdarray<double, 2> kpoints(3, numkp);
        std::vector<double> kpoint_weights(numkp);

        for (int i0 = 0; i0 < ngridk[0]; i0++) 
        {
            for (int i1 = 0; i1 < ngridk[1]; i1++) 
            {
                for (int i2 = 0; i2 < ngridk[2]; i2++)
                {
                    kpoints(0, ik) = double(i0) / ngridk[0];
                    kpoints(1, ik) = double(i1) / ngridk[1];
                    kpoints(2, ik) = double(i2) / ngridk[2];
                    kpoint_weights[ik] = 1.0 / numkp;
                    ik++;
                }
            }
        }

        K_set ks(parameters);
        ks.add_kpoints(kpoints, &kpoint_weights[0]);
        ks.initialize();
        
        Density* density = new Density(parameters);
        density->allocate();
        
        if (Utils::file_exists(storage_file_name))
        {
            density->load();
            potential->load();
        }
        else
        {
            density->initial_density();
        }

        DFT_ground_state dft(parameters, potential, density, &ks);
        double potential_tol = parser["potential_tol"].get(1e-4);
        double energy_tol = parser["energy_tol"].get(1e-4);

        dft.scf_loop(potential_tol, energy_tol, parser["num_dft_iter"].get(100));

        //dft.relax_atom_positions();

        //parameters.write_json_output();

        delete density;
        delete potential;
        
        parameters.clear();

        Timer::print();
    }

    Platform::barrier();
    Platform::finalize();
}
