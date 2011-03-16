#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <algorithm>
#include <vector>
#include <utility>
#include <string>
#include <cstring>

#include <psifiles.h>
#include <physconst.h>
#include <libciomr/libciomr.h>
#include <libpsio/psio.hpp>
#include <libchkpt/chkpt.hpp>
#include <libiwl/iwl.hpp>
#include <libqt/qt.h>

#include <libmints/mints.h>
#include <libfunctional/superfunctional.h>
#include "ks.h"
#include "integralfunctors.h"

using namespace std;
using namespace psi;
using namespace psi::functional;
using namespace boost;

namespace psi { namespace scf {

KS::KS(Options & options, shared_ptr<PSIO> psio) :
    options_(options), psio_(psio)
{
    common_init();
}
KS::~KS()
{
}
void KS::common_init()
{
    // Take the molecule from the environment
    molecule_ = Process::environment.molecule();

    // Load in the basis set
    shared_ptr<BasisSetParser> parser(new Gaussian94BasisSetParser());
    basisset_ = BasisSet::construct(parser, molecule_, "BASIS");

    // Build the integrator
    int block_size = options_.get_int("DFT_BLOCK_SIZE");
    integrator_ = Integrator::build_integrator(molecule_, psio_, options_);
    integrator_->buildGrid(block_size);

    //Build the superfunctional
    functional_ = SuperFunctional::createSuperFunctional(options_.get_str("DFT_FUNCTIONAL"),block_size,1);

    // Temporary print, to make sure we're in the right spot
    fprintf(outfile,"\n  Selected Functional is %s.\n",functional_->getName().c_str());

    //Grab the properties object for this basis
    properties_ = shared_ptr<Properties> (new Properties(basisset_,block_size));
    properties_->setCutoffEpsilon(options_.get_double("DFT_BASIS_EPSILON"));
    //Always need rho
    properties_->setToComputeDensity(true);
    if (functional_->isGGA()) {
        //Sometimes need gamma
        properties_->setToComputeDensityGradient(true);
    }
    if (functional_->isMeta()) {
        //sometimes need tau
        properties_->setToComputeKEDensity(true);
    }
}
RKS::RKS(Options & options, shared_ptr<PSIO> psio, shared_ptr<Chkpt> chkpt) :
    RHF(options, psio, chkpt), KS(options,psio)
{
    common_init();
}
RKS::RKS(Options & options, shared_ptr<PSIO> psio) :
    RHF(options, psio), KS(options,psio)
{
    common_init();
}
void RKS::common_init()
{
    V_ = factory_->create_shared_matrix("Va (Kohn-Sham Potential)");
}
RKS::~RKS()
{
}
void RKS::form_V()
{
    // Zero the V_ matrix
    V_->zero();

    // Some temporary buffers
    shared_ptr<Matrix> Vt1 = factory_->create_shared_matrix("V Temp 1");
    shared_ptr<Matrix> Vt2 = factory_->create_shared_matrix("V Temp 2");
    double** V_temp1 = Vt1->pointer();
    double** V_temp2 = Vt2->pointer();

    // Zero the registers
    double functional_E = 0.0;
    double densityCheck = 0.0;
    double dipoleCheckX = 0.0;
    double dipoleCheckY = 0.0;
    double dipoleCheckZ = 0.0;

    // Grab the integrator's grid block
    int nblocks = integrator_->getNBlocks();
    double *x;
    double *y;
    double *z;
    double *w;

    // Grab the properties references
    double *rho_a       = properties_->getRhoA();
    double *gamma_aa    = properties_->getGammaAA();
    double *tau_a       = properties_->getTauA();
    double *rho_x       = properties_->getRhoAX();
    double *rho_y       = properties_->getRhoAY();
    double *rho_z       = properties_->getRhoAZ();
    // (These will not be used unless needed)

    // Grab sparseness information and scratch array
    int* rel2abs = properties_->rel2absFunctions();
    double **scratch    = properties_->getScratch(); //Already allocated

    // Grab the basis points references
    double **bas = properties_->getPoints();
    double **bas_x = properties_->getGradientsX();
    double **bas_y = properties_->getGradientsY();
    double **bas_z = properties_->getGradientsZ();
    // (These will not be used unless needed)

    // Grab the functional references
    double *zk          = functional_->getFunctionalValue();
    double *v_rho_a     = functional_->getV_RhoA();
    double *v_gamma_aa  = functional_->getV_GammaAA();
    double *v_tau_a     = functional_->getV_TauA();
    // (These will not be used unless needed)

    // GGA? Meta?
    bool GGA = functional_->isGGA();
    bool Meta = functional_->isMeta();

    // Some indexing
    int ntrue, nsigf, nbf, index, offset, h, mu, nu, m ,n;
    double contribution;

    nbf = KS::basisset_->nbf();

    // Traverse grid blocks
    for (int N = 0; N < nblocks; N++) {

        // Compute integration points
        shared_ptr<GridBlock> block = integrator_->getBlock(N);
        x = block->getX();
        y = block->getY();
        z = block->getZ();
        w = block->getWeights();
        ntrue = block->getTruePoints();

        // Compute properties and basis points
        properties_->computeRKSProperties(block, D_, Ca_, doccpi_);
        nsigf = properties_->nSignificantFunctions();

        // Compute functional values and partials
        functional_->computeRKSFunctional(properties_);

        // Roll the weights in
        for (index = 0; index < ntrue; index++) {
            zk[index] *= w[index];
            v_rho_a[index] *= w[index];
        }
        if (GGA) {
            for (index = 0; index < ntrue; index++) {
                v_gamma_aa[index] *= w[index];
            }
        }
        if (Meta) {
            for (index = 0; index < ntrue; index++) {
                v_tau_a[index] *= w[index];
            }
        }

        // Compute functional energy
        // And monopole/dipole checks
        for (index = 0; index < ntrue; index++) {
            //printf(" Block %d, Point %d, rho %14.10E\n", N, index, rho_a[index]);

            functional_E += zk[index];
            densityCheck += w[index]*rho_a[index];
            dipoleCheckX += w[index]*x[index]*rho_a[index];
            dipoleCheckY += w[index]*y[index]*rho_a[index];
            dipoleCheckZ += w[index]*z[index]*rho_a[index];
        }

        // LSDA contribution to potential:
        // V_mn += v_rho_a * phi_m * phi_n
        for (index = 0; index < ntrue; index++) {
            memcpy((void*) &scratch[index][0], (void*) &bas[index][0], nsigf*sizeof(double));
            C_DSCAL(nsigf, v_rho_a[index], &scratch[index][0], 1);
        }
        C_DGEMM('T','N', nsigf, nsigf, ntrue, 1.0, &scratch[0][0], nbf, \
            &bas[0][0], nbf, 0.0, &V_temp1[0][0], nbf);

        // GGA contribution to potential:
        //V_mn += v_gamma_aa * \nabla rho_a \dot \nabla (phi_m * phi_n) (for RKS, v_gamma_aa is already tuned in)
        if (GGA) {
            // Gradient x
            for (index = 0; index < ntrue; index++) {
                memcpy((void*) &scratch[index][0], (void*) &bas[index][0], nsigf*sizeof(double));
                C_DSCAL(nsigf, v_gamma_aa[index]*rho_x[index], &scratch[index][0], 1);
            }
            C_DGEMM('T','N', nsigf, nsigf, ntrue, 1.0, &scratch[0][0], nbf, \
                &bas_x[0][0], nbf, 0.0, &V_temp2[0][0], nbf);

            // Gradient y
            for (index = 0; index < ntrue; index++) {
                memcpy((void*) &scratch[index][0], (void*) &bas[index][0], nsigf*sizeof(double));
                C_DSCAL(nsigf, v_gamma_aa[index]*rho_y[index], &scratch[index][0], 1);
            }
            C_DGEMM('T','N', nsigf, nsigf, ntrue, 1.0, &scratch[0][0], nbf, \
                &bas_y[0][0], nbf, 1.0, &V_temp2[0][0], nbf);

            // Gradient z
            for (index = 0; index < ntrue; index++) {
                memcpy((void*) &scratch[index][0], (void*) &bas[index][0], nsigf*sizeof(double));
                C_DSCAL(nsigf, v_gamma_aa[index]*rho_z[index], &scratch[index][0], 1);
            }
            C_DGEMM('T','N', nsigf, nsigf, ntrue, 1.0, &scratch[0][0], nbf, \
                &bas_z[0][0], nbf, 1.0, &V_temp2[0][0], nbf);

            // I only did the left term, so V_xc_GGA = V_temp2_ + v_temp2_':
            for (int m = 0; m < nsigf; m++)
                for (int n = 0; n < nsigf; n++)
                    V_temp1[m][n] += V_temp2[m][n] + V_temp2[n][m];

        }

        // Meta contribution to potential
        //V_mn += 2 * v_tau_a * \nabla \phi_m \dot \nabla \phi_n
        if (Meta) {
            // phi_x
            for (index = 0; index < ntrue; index++) {
                memcpy((void*) &scratch[index][0], (void*) &bas_x[index][0], nsigf*sizeof(double));
                C_DSCAL(nsigf, 2.0*v_tau_a[index], &scratch[index][0], 1);
            }
            C_DGEMM('T','N', nsigf, nsigf, ntrue, 1.0, &scratch[0][0], nbf, \
                &bas_x[0][0], nbf, 1.0, &V_temp1[0][0], nbf);

            // phi_y
            for (index = 0; index < ntrue; index++) {
                memcpy((void*) &scratch[index][0], (void*) &bas_y[index][0], nsigf*sizeof(double));
                C_DSCAL(nsigf, 2.0*v_tau_a[index], &scratch[index][0], 1);
            }
            C_DGEMM('T','N', nsigf, nsigf, ntrue, 1.0, &scratch[0][0], nbf, \
                &bas_y[0][0], nbf, 1.0, &V_temp1[0][0], nbf);

            // phi_z
            for (index = 0; index < ntrue; index++) {
                memcpy((void*) &scratch[index][0], (void*) &bas_z[index][0], nsigf*sizeof(double));
                C_DSCAL(nsigf, 2.0*v_tau_a[index], &scratch[index][0], 1);
            }
            C_DGEMM('T','N', nsigf, nsigf, ntrue, 1.0, &scratch[0][0], nbf, \
                &bas_z[0][0], nbf, 1.0, &V_temp1[0][0], nbf);
        }

        // Add into the global Vxc matrix
        for (int m = 0; m < nsigf; m++)
            for (int n = 0; n <= m; n++) {
                V_->add(0, rel2abs[m], rel2abs[n], V_temp1[m][n]);
                if (m!=n)
                    V_->add(0, rel2abs[n], rel2abs[m], V_temp1[n][m]);
            }

    } // End traverse over grid blocks

    // Now count beta electrons
    densityCheck *= 2.0;
    dipoleCheckX *= 2.0;
    dipoleCheckY *= 2.0;
    dipoleCheckZ *= 2.0;

    quad_values_["E_xc"] = functional_E;
    quad_values_["<rho>"] = densityCheck;
    quad_values_["<rho*x>"] = dipoleCheckX;
    quad_values_["<rho*y>"] = dipoleCheckX;
    quad_values_["<rho*z>"] = dipoleCheckX;

    if (print_ > 2) {
        fprintf(outfile,"\n\n  @RKS Numerical Density: %14.10f\n",densityCheck);
        fprintf(outfile,"  @RKS Numerical Dipole: <%14.10f,%14.10f,%14.10f>\n",dipoleCheckX,dipoleCheckY,dipoleCheckZ);
    }
}
void RKS::form_G()
{
    form_V();
    if (!functional_->isHybrid()) {
        // This will build J (stored in G)
        J_Functor j_builder(G_, D_);
        process_tei<J_Functor>(j_builder);
        J_->copy(G_);

        G_->scale(2.0);
        G_->add(V_);

    } else {
        // This will build J (stored in G) and K
        J_K_Functor jk_builder(G_, K_, D_, Ca_, nalphapi_);
        process_tei<J_K_Functor>(jk_builder);
        J_->copy(G_);

        double alpha = functional_->getExactExchange();
        G_->scale(2.0);
        K_->scale(alpha);
        G_->subtract(K_);
        K_->scale(1.0/alpha);
        G_->add(V_);
    }
}
double RKS::compute_E()
{
    // E_DFT = 2.0 D*H + 2.0 D*J - \alpha D*K + E_xc
    double one_electron_E = 2.0*D_->vector_dot(H_);
    double coulomb_E = 2.0*D_->vector_dot(J_);

    double exchange_E = 0.0;
    if (functional_->isHybrid()) {
        exchange_E = -functional_->getExactExchange()*D_->vector_dot(K_);
    }
    double Etotal = 0.0;
    Etotal += nuclearrep_;
    Etotal += one_electron_E;
    Etotal += coulomb_E;
    Etotal += exchange_E;
    Etotal += quad_values_["E_xc"];
    if (functional_->isDashD()) {
        double dashD_E = functional_->getDashD()->computeEnergy(HF::molecule_);
        Etotal += dashD_E;
    }

    return Etotal;
}
UKS::UKS(Options & options, shared_ptr<PSIO> psio, shared_ptr<Chkpt> chkpt) :
    UHF(options, psio, chkpt), KS(options,psio)
{
    common_init();
}
UKS::UKS(Options & options, shared_ptr<PSIO> psio) :
    UHF(options, psio), KS(options,psio)
{
    common_init();
}
void UKS::common_init()
{
    Va_ = factory_->create_shared_matrix("Va (Kohn-Sham Potential)");
    Vb_ = factory_->create_shared_matrix("Vb (Kohn-Sham Potential)");
}
UKS::~UKS()
{
}
void UKS::form_V()
{
    // Zero the V_ matrix
    Va_->zero();
    Vb_->zero();

    // Some temporary buffers
    shared_ptr<Matrix> Vt1a = factory_->create_shared_matrix("V Temp 1 (Alpha)");
    shared_ptr<Matrix> Vt2a = factory_->create_shared_matrix("V Temp 2 (Alpha)");
    double** V_temp1a = Vt1a->pointer();
    double** V_temp2a = Vt2a->pointer();
    shared_ptr<Matrix> Vt1b = factory_->create_shared_matrix("V Temp 1 (Beta)");
    shared_ptr<Matrix> Vt2b = factory_->create_shared_matrix("V Temp 2 (Beta)");
    double** V_temp1b = Vt1b->pointer();
    double** V_temp2b = Vt2b->pointer();

    // Zero the registers
    double functional_E = 0.0;
    double densityCheckA = 0.0;
    double dipoleCheckXA = 0.0;
    double dipoleCheckYA = 0.0;
    double dipoleCheckZA = 0.0;
    double densityCheckB = 0.0;
    double dipoleCheckXB = 0.0;
    double dipoleCheckYB = 0.0;
    double dipoleCheckZB = 0.0;

    // Grab the integrator's grid block
    int nblocks = integrator_->getNBlocks();
    double *x;
    double *y;
    double *z;
    double *w;

    // Grab the properties references
    double *rho_a       = properties_->getRhoA();
    double *rho_b       = properties_->getRhoB();
    double *gamma_aa    = properties_->getGammaAA();
    double *gamma_ab    = properties_->getGammaAB();
    double *gamma_bb    = properties_->getGammaBB();
    double *tau_a       = properties_->getTauA();
    double *tau_b       = properties_->getTauB();

    double *rho_ax       = properties_->getRhoAX();
    double *rho_ay       = properties_->getRhoAY();
    double *rho_az       = properties_->getRhoAZ();
    double *rho_bx       = properties_->getRhoBX();
    double *rho_by       = properties_->getRhoBY();
    double *rho_bz       = properties_->getRhoBZ();
    // (These will not be used unless needed)

    // Grab sparseness information and scratch array
    int* rel2abs = properties_->rel2absFunctions();
    double **scratch    = properties_->getScratch(); //Already allocated

    // Grab the basis points references
    double **bas = properties_->getPoints();
    double **bas_x = properties_->getGradientsX();
    double **bas_y = properties_->getGradientsY();
    double **bas_z = properties_->getGradientsZ();
    // (These will not be used unless needed)

    // Grab the functional references
    double *zk          = functional_->getFunctionalValue();
    double *v_rho_a     = functional_->getV_RhoA();
    double *v_rho_b     = functional_->getV_RhoB();
    double *v_gamma_aa  = functional_->getV_GammaAA();
    double *v_gamma_ab  = functional_->getV_GammaAB();
    double *v_gamma_bb  = functional_->getV_GammaBB();
    double *v_tau_a     = functional_->getV_TauA();
    double *v_tau_b     = functional_->getV_TauB();
    // (These will not be used unless needed)

    // GGA? Meta?
    bool GGA = functional_->isGGA();
    bool Meta = functional_->isMeta();

    // Some indexing
    int ntrue, nsigf, nbf, index, offset, h, mu, nu, m ,n;
    double contribution;

    nbf = KS::basisset_->nbf();

    // Traverse grid blocks
    for (int N = 0; N < nblocks; N++) {

        // Compute integration points
        shared_ptr<GridBlock> block = integrator_->getBlock(N);
        x = block->getX();
        y = block->getY();
        z = block->getZ();
        w = block->getWeights();
        ntrue = block->getTruePoints();

        // Compute properties and basis points
        properties_->computeUKSProperties(block, Da_, Db_, Ca_, Cb_, nalphapi_, nalphapi_);
        nsigf = properties_->nSignificantFunctions();

        // Compute functional values and partials
        functional_->computeUKSFunctional(properties_);

        for (index = 0; index < ntrue; index++) {
            zk[index] *= w[index];
            v_rho_a[index] *= w[index];
            v_rho_b[index] *= w[index];
        }
        if (GGA) {
            for (index = 0; index < ntrue; index++) {
                v_gamma_aa[index] *= w[index];
                v_gamma_ab[index] *= w[index];
                v_gamma_bb[index] *= w[index];
            }
        }
        if (Meta) {
            for (index = 0; index < ntrue; index++) {
                v_tau_a[index] *= w[index];
                v_tau_b[index] *= w[index];
            }
        }

        // Compute functional energy
        // And monopole/dipole checks
        for (index = 0; index < ntrue; index++) {
            //printf(" Block %d, Point %d, rho %14.10E\n", N, index, rho_a[index]);

            functional_E += zk[index];
            densityCheckA += w[index]*rho_a[index];
            dipoleCheckXA += w[index]*x[index]*rho_a[index];
            dipoleCheckYA += w[index]*y[index]*rho_a[index];
            dipoleCheckZA += w[index]*z[index]*rho_a[index];
            densityCheckB += w[index]*rho_b[index];
            dipoleCheckXB += w[index]*x[index]*rho_b[index];
            dipoleCheckYB += w[index]*y[index]*rho_b[index];
            dipoleCheckZB += w[index]*z[index]*rho_b[index];
        }

        // LSDA contribution to potential:
        // Alpha
        // V_mn^a += v_rho_a * phi_m * phi_n
        for (index = 0; index < ntrue; index++) {
            memcpy((void*) &scratch[index][0], (void*) &bas[index][0], nsigf*sizeof(double));
            C_DSCAL(nsigf, v_rho_a[index], &scratch[index][0], 1);
        }
        C_DGEMM('T','N', nsigf, nsigf, ntrue, 1.0, &scratch[0][0], nbf, \
            &bas[0][0], nbf, 0.0, &V_temp1a[0][0], nbf);
        // Beta
        // V_mn^b += v_rho_b * phi_m * phi_n
        for (index = 0; index < ntrue; index++) {
            memcpy((void*) &scratch[index][0], (void*) &bas[index][0], nsigf*sizeof(double));
            C_DSCAL(nsigf, v_rho_b[index], &scratch[index][0], 1);
        }
        C_DGEMM('T','N', nsigf, nsigf, ntrue, 1.0, &scratch[0][0], nbf, \
            &bas[0][0], nbf, 0.0, &V_temp1b[0][0], nbf);

        // GGA contribution to potential:
        //V_mn^a += [2 * v_gamma_aa * \nabla rho_a + v_gamma_ab * \nabla rho_b] \dot \nabla (phi_m * phi_n)
        if (GGA) {
            // Alpha
            // Gradient x
            for (index = 0; index < ntrue; index++) {
                memcpy((void*) &scratch[index][0], (void*) &bas[index][0], nsigf*sizeof(double));
                C_DSCAL(nsigf, 2.0*v_gamma_aa[index]*rho_ax[index] + v_gamma_ab[index]*rho_bx[index], &scratch[index][0], 1);
            }
            C_DGEMM('T','N', nsigf, nsigf, ntrue, 1.0, &scratch[0][0], nbf, \
                &bas_x[0][0], nbf, 0.0, &V_temp2a[0][0], nbf);

            // Gradient y
            for (index = 0; index < ntrue; index++) {
                memcpy((void*) &scratch[index][0], (void*) &bas[index][0], nsigf*sizeof(double));
                C_DSCAL(nsigf, 2.0*v_gamma_aa[index]*rho_ay[index] + v_gamma_ab[index]*rho_by[index], &scratch[index][0], 1);
            }
            C_DGEMM('T','N', nsigf, nsigf, ntrue, 1.0, &scratch[0][0], nbf, \
                &bas_y[0][0], nbf, 1.0, &V_temp2a[0][0], nbf);

            // Gradient z
            for (index = 0; index < ntrue; index++) {
                memcpy((void*) &scratch[index][0], (void*) &bas[index][0], nsigf*sizeof(double));
                C_DSCAL(nsigf, 2.0*v_gamma_aa[index]*rho_az[index] + v_gamma_ab[index]*rho_bz[index], &scratch[index][0], 1);
            }
            C_DGEMM('T','N', nsigf, nsigf, ntrue, 1.0, &scratch[0][0], nbf, \
                &bas_z[0][0], nbf, 1.0, &V_temp2a[0][0], nbf);

            // I only did the left term, so V_xc_GGA = V_temp2_ + v_temp2_':
            for (int m = 0; m < nsigf; m++)
                for (int n = 0; n < nsigf; n++)
                    V_temp1a[m][n] += V_temp2a[m][n] + V_temp2a[n][m];

            // Beta
            // Gradient x
            for (index = 0; index < ntrue; index++) {
                memcpy((void*) &scratch[index][0], (void*) &bas[index][0], nsigf*sizeof(double));
                C_DSCAL(nsigf, 2.0*v_gamma_bb[index]*rho_bx[index] + v_gamma_ab[index]*rho_ax[index], &scratch[index][0], 1);
            }
            C_DGEMM('T','N', nsigf, nsigf, ntrue, 1.0, &scratch[0][0], nbf, \
                &bas_x[0][0], nbf, 0.0, &V_temp2b[0][0], nbf);

            // Gradient y
            for (index = 0; index < ntrue; index++) {
                memcpy((void*) &scratch[index][0], (void*) &bas[index][0], nsigf*sizeof(double));
                C_DSCAL(nsigf, 2.0*v_gamma_bb[index]*rho_by[index] + v_gamma_ab[index]*rho_ay[index], &scratch[index][0], 1);
            }
            C_DGEMM('T','N', nsigf, nsigf, ntrue, 1.0, &scratch[0][0], nbf, \
                &bas_y[0][0], nbf, 1.0, &V_temp2b[0][0], nbf);

            // Gradient z
            for (index = 0; index < ntrue; index++) {
                memcpy((void*) &scratch[index][0], (void*) &bas[index][0], nsigf*sizeof(double));
                C_DSCAL(nsigf, 2.0*v_gamma_bb[index]*rho_bz[index] + v_gamma_ab[index]*rho_az[index], &scratch[index][0], 1);
            }
            C_DGEMM('T','N', nsigf, nsigf, ntrue, 1.0, &scratch[0][0], nbf, \
                &bas_z[0][0], nbf, 1.0, &V_temp2b[0][0], nbf);

            // I only did the left term, so V_xc_GGA = V_temp2_ + v_temp2_':
            for (int m = 0; m < nsigf; m++)
                for (int n = 0; n < nsigf; n++)
                    V_temp1b[m][n] += V_temp2b[m][n] + V_temp2b[n][m];

        }

        // TODO determine if the 2.0 is still here for meta-UKS
        // Meta contribution to potential
        //V_mn += 2 * v_tau_a * \nabla \phi_m \dot \nabla \phi_n
        if (Meta) {
            // Alpha
            // phi_x
            for (index = 0; index < ntrue; index++) {
                memcpy((void*) &scratch[index][0], (void*) &bas_x[index][0], nsigf*sizeof(double));
                C_DSCAL(nsigf, 2.0*v_tau_a[index], &scratch[index][0], 1);
            }
            C_DGEMM('T','N', nsigf, nsigf, ntrue, 1.0, &scratch[0][0], nbf, \
                &bas_x[0][0], nbf, 1.0, &V_temp1a[0][0], nbf);

            // phi_y
            for (index = 0; index < ntrue; index++) {
                memcpy((void*) &scratch[index][0], (void*) &bas_y[index][0], nsigf*sizeof(double));
                C_DSCAL(nsigf, 2.0*v_tau_a[index], &scratch[index][0], 1);
            }
            C_DGEMM('T','N', nsigf, nsigf, ntrue, 1.0, &scratch[0][0], nbf, \
                &bas_y[0][0], nbf, 1.0, &V_temp1a[0][0], nbf);

            // phi_z
            for (index = 0; index < ntrue; index++) {
                memcpy((void*) &scratch[index][0], (void*) &bas_z[index][0], nsigf*sizeof(double));
                C_DSCAL(nsigf, 2.0*v_tau_a[index], &scratch[index][0], 1);
            }
            C_DGEMM('T','N', nsigf, nsigf, ntrue, 1.0, &scratch[0][0], nbf, \
                &bas_z[0][0], nbf, 1.0, &V_temp1a[0][0], nbf);

            // Beta
            // phi_x
            for (index = 0; index < ntrue; index++) {
                memcpy((void*) &scratch[index][0], (void*) &bas_x[index][0], nsigf*sizeof(double));
                C_DSCAL(nsigf, 2.0*v_tau_b[index], &scratch[index][0], 1);
            }
            C_DGEMM('T','N', nsigf, nsigf, ntrue, 1.0, &scratch[0][0], nbf, \
                &bas_x[0][0], nbf, 1.0, &V_temp1b[0][0], nbf);

            // phi_y
            for (index = 0; index < ntrue; index++) {
                memcpy((void*) &scratch[index][0], (void*) &bas_y[index][0], nsigf*sizeof(double));
                C_DSCAL(nsigf, 2.0*v_tau_b[index], &scratch[index][0], 1);
            }
            C_DGEMM('T','N', nsigf, nsigf, ntrue, 1.0, &scratch[0][0], nbf, \
                &bas_y[0][0], nbf, 1.0, &V_temp1b[0][0], nbf);

            // phi_z
            for (index = 0; index < ntrue; index++) {
                memcpy((void*) &scratch[index][0], (void*) &bas_z[index][0], nsigf*sizeof(double));
                C_DSCAL(nsigf, 2.0*v_tau_b[index], &scratch[index][0], 1);
            }
            C_DGEMM('T','N', nsigf, nsigf, ntrue, 1.0, &scratch[0][0], nbf, \
                &bas_z[0][0], nbf, 1.0, &V_temp1b[0][0], nbf);
        }

        // Add into the global Vxc matrix
        for (int m = 0; m < nsigf; m++)
            for (int n = 0; n <= m; n++) {
                Va_->add(0, rel2abs[m], rel2abs[n], V_temp1a[m][n]);
                if (m!=n)
                    Va_->add(0, rel2abs[n], rel2abs[m], V_temp1a[n][m]);
            }
        for (int m = 0; m < nsigf; m++)
            for (int n = 0; n <= m; n++) {
                Vb_->add(0, rel2abs[m], rel2abs[n], V_temp1b[m][n]);
                if (m!=n)
                    Vb_->add(0, rel2abs[n], rel2abs[m], V_temp1b[n][m]);
            }

    } // End traverse over grid blocks

    quad_values_["E_xc"] = functional_E;
    quad_values_["<rho_a>"] = densityCheckA;
    quad_values_["<rho_a*x>"] = dipoleCheckXA;
    quad_values_["<rho_a*y>"] = dipoleCheckYA;
    quad_values_["<rho_a*z>"] = dipoleCheckZA;
    quad_values_["<rho_b>"] = densityCheckB;
    quad_values_["<rho_b*x>"] = dipoleCheckXB;
    quad_values_["<rho_b*y>"] = dipoleCheckYB;
    quad_values_["<rho_b*z>"] = dipoleCheckZB;

    if (print_ > 2) {
        fprintf(outfile,"\n\n  @RKS Numerical Alpha Density: %14.10f\n",densityCheckA);
        fprintf(outfile,"  @RKS Numerical Beta Density: %14.10f\n",densityCheckB);
        fprintf(outfile,"  @RKS Numerical Alpha Dipole: <%14.10f,%14.10f,%14.10f>\n",dipoleCheckXA,dipoleCheckYA,dipoleCheckZA);
        fprintf(outfile,"  @RKS Numerical Beta Dipole:  <%14.10f,%14.10f,%14.10f>\n",dipoleCheckXB,dipoleCheckYB,dipoleCheckZB);
    }
}
void UKS::form_G()
{
    form_V();
    if (!functional_->isHybrid()) {
        // This will build J (stored in G)
        J_Functor j_builder(Ga_, Da_);
        process_tei<J_Functor>(j_builder);
        J_->copy(Ga_);

        Gb_->copy(Ga_);
        Ga_->add(Va_);
        Gb_->add(Vb_);

    } else {
        // This will build J (stored in G) and K
        J_Ka_Kb_Functor jk_builder(Ga_, Ka_, Kb_, Da_, Db_, Ca_, Cb_, nalphapi_, nbetapi_);
        process_tei<J_Ka_Kb_Functor>(jk_builder);
        J_->copy(Ga_);
        Gb_->copy(Ga_);

        double alpha = functional_->getExactExchange();
        Ka_->scale(alpha);
        Kb_->scale(alpha);
        Ga_->subtract(Ka_);
        Gb_->subtract(Kb_);
        Ka_->scale(1.0/alpha);
        Kb_->scale(1.0/alpha);
        Ga_->add(Va_);
        Gb_->add(Vb_);
    }

    //J_->print(outfile);
    //Ka_->print(outfile);
    //Kb_->print(outfile);
    //Va_->print();
    //Vb_->print();
}
double UKS::compute_E()
{
    //Fa_->print(outfile);
    //Fb_->print(outfile);
    //Ca_->print(outfile);
    //Cb_->print(outfile);

    // E_DFT = 2.0 D*H + 2.0 D*J - \alpha D*K + E_xc
    double one_electron_E = Da_->vector_dot(H_);
    one_electron_E += Db_->vector_dot(H_);
    double coulomb_E = Da_->vector_dot(J_);
    coulomb_E += Db_->vector_dot(J_);

    double exchange_E = 0.0;
    if (functional_->isHybrid()) {
        exchange_E = -functional_->getExactExchange()*Da_->vector_dot(Ka_);
        exchange_E = -functional_->getExactExchange()*Db_->vector_dot(Kb_);
    }
    double Etotal = 0.0;
    Etotal += nuclearrep_;
    Etotal += one_electron_E;
    Etotal += 0.5*coulomb_E;
    Etotal += exchange_E;
    Etotal += quad_values_["E_xc"];
    if (functional_->isDashD()) {
        double dashD_E = functional_->getDashD()->computeEnergy(HF::molecule_);
        Etotal += dashD_E;
    }

    if (print_ > 2) {
        fprintf(outfile, "Nuclear Repulsion energy: %24.16f\n", nuclearrep_);
        fprintf(outfile, "One-electron energy:      %24.16f\n", one_electron_E);
        fprintf(outfile, "Coulomb energy:           %24.16f\n", 0.5*coulomb_E);
        fprintf(outfile, "Exchange energy:          %24.16f\n", exchange_E);
        fprintf(outfile, "Functional energy:        %24.16f\n", quad_values_["E_xc"]);
    }
    return Etotal;
}


}}