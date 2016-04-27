#ifndef DIAGNOSTICPROBES_H
#define DIAGNOSTICPROBES_H

#include <hdf5.h>

#include "Diagnostic.h"

#include "Params.h"
#include "Patch.h"
#include "SmileiMPI.h"

#include "Field2D.h"

class DiagnosticProbes : public Diagnostic {
    friend class SmileiMPI;

public :
    
    //! Default constructor
    DiagnosticProbes( Params &params, SmileiMPI* smpi, Patch* patch, int diagId );
    //! Cloning constructor
    DiagnosticProbes(DiagnosticProbes*, Params&, Patch* );
    //! Default destructor
    ~DiagnosticProbes();
    
    void initParticles(Params&, Patch *);
    
    virtual void openFile( Params& params, SmileiMPI* smpi, VectorPatch& vecPatches, bool newfile );
    
    virtual void closeFile();
    
    virtual bool prepare( Patch* patch, int timestep );
    
    virtual void run( Patch* patch, int timestep );
    
    virtual void write(int timestep);
    
    
    void setFileSplitting( Params& params, SmileiMPI* smpi, VectorPatch& vecPatches );
    
    void writePositionIn( Params &params );
    void writePositions( int ndim_Particles, int probeDim, hid_t group_id );
    void compute(unsigned int timestep, ElectroMagn* EMfields);
    
    int getLastPartId() {
        return probesStart+probeParticles.size();
    }
    

private :
    //int probeId_;
    
    //! Dimension of the probe grid
    unsigned int dimProbe;
    
    //! Number of points in each dimension
    std::vector<unsigned int> vecNumber; 
    
    //! List of the coordinates of the probe vertices
    std::vector< std::vector<double> > allPos;
    
    //! fake particles acting as probes
    Particles probeParticles;
    
    //! each probe will write in a buffer
    Field2D* probesArray;
    
    int probesStart;
    
    //! number of fake particles for each probe diagnostic
    unsigned int nPart_total;
    
    //! Number of fields to save
    int nFields;
    
    //! List of fields to save
    std::vector<std::string> fieldname;
    
    //! Indices in the output array where each field goes
    std::vector<unsigned int> fieldlocation;
    
    
    //! return name of the probe based on its number
    std::string probeName();
    
    //! E local fields for the projector
    LocalFields Eloc_fields;
    //! B local fields for the projector
    LocalFields Bloc_fields;
    //! J local fields for the projector
    LocalFields Jloc_fields;
    //! Rho local field for the projector
    double Rloc_fields;
    
    Interpolator* interp_;
    
};

#endif
