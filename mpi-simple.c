#include <stdio.h>
#include <math.h>
#include <time.h>
#include <stdlib.h>
#include <stdbool.h>
#include <unistd.h>
#include <mpi.h>

int asprintf(char **strp, const char *fmt, ...);


/**
 * @brief Calculates the time difference in seconds between two timespec structs
 * @return the time difference in seconds between the two structs
 */
long double toSeconds(struct timespec start, struct timespec end) {
    long long difSec = end.tv_sec - start.tv_sec;
    long long difNSec = end.tv_nsec - start.tv_nsec;
    long long totalNS = difSec*1000000000L + difNSec;
    return (long double) totalNS / 1000000000.0;
}

// TODO correct this docstring
/**
 * @brief Mallocs memory for a n*rows 2D array
 * @param n the size of each row in the 2D array
 * @param rows the number of rows in the 2D array
 * @return a pointer to the 2D array
 */
double** newSubPlane(unsigned int n, unsigned int rows) {
    double** plane  = ( double** )malloc(rows * sizeof(double*));
    plane[0] = ( double * )malloc(rows * n * sizeof(double));

    for(unsigned int i = 0; i<rows; i++)
        plane[i] = (*plane + n * i);

    return plane;
}

/**
 * @brief Populates the plane's walls with the values provided, and zero's out the rest.
 */
void populateSubPlane(double** plane, int sizeOfPlane, int numRows, double top, double bottom, double farLeft, double farRight, int world_rank, int world_size)
{   
    for(int i=0; i<numRows; i++) {
        for(int j=0; j<sizeOfPlane; j++) {
            if(j == 0) {
                // Left
                plane[i][j] = farLeft;
            } else if(i == 0 && world_rank == 0) {
                // Top
                plane[i][j] = top;
            } else if(j == sizeOfPlane-1) {
                // Right
                plane[i][j] = farRight;
            } else if(i == numRows-1 && world_rank == world_size-1) {
                // Bottom
                plane[i][j] = bottom;
            } else {
                plane[i][j] = 0;
            }
        }
    }
}

// TODO propper doc string
// Runs the relaxation technique on the 2d array of doubles that it is passed.
unsigned long relaxPlane(double** plane, int numRows, int sizeOfPlane, double tolerance, int world_rank, int world_size)
{

    unsigned long iterations = 0;
    int i, j, endFlag;
    double pVal;

    int sizeOfInner = sizeOfPlane-2;
    int sendBot = numRows-2; 
    int recBot = numRows-1;

    do {
        endFlag = true;
        iterations++;

        for(i=1; i<recBot; i++) {
            for(j=1; j<sizeOfPlane-1; j++) {
                pVal = plane[i][j];
                plane[i][j] = (plane[i-1][j] + plane[i+1][j] + plane[i][j-1] + plane[i][j+1])/4;
                if(endFlag && tolerance < fabs(plane[i][j]-pVal)) {
                    endFlag = false;
                }
            }
        }
        
        // Update process that needs the new data
        if(world_rank==0) {
            // Only send data down
            MPI_Send(&plane[sendBot][1], sizeOfInner, MPI_DOUBLE, 1, 0, MPI_COMM_WORLD);
            MPI_Recv(&plane[recBot][1], sizeOfInner, MPI_DOUBLE, 1, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        } else if(world_rank==world_size-1) {
            // Only send data up
            MPI_Send(&plane[1][1], sizeOfInner, MPI_DOUBLE, world_size-2, 0, MPI_COMM_WORLD);
            MPI_Recv(&plane[0][1], sizeOfInner, MPI_DOUBLE, world_size-2, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        } else {
            // Send data up and down
            MPI_Send(&plane[1][1], sizeOfInner, MPI_DOUBLE, world_rank-1, 0, MPI_COMM_WORLD);
            MPI_Send(&plane[sendBot][1], sizeOfInner, MPI_DOUBLE, world_rank+1, 0, MPI_COMM_WORLD);
            MPI_Recv(&plane[0][1], sizeOfInner, MPI_DOUBLE, world_rank-1, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
            MPI_Recv(&plane[recBot][1], sizeOfInner, MPI_DOUBLE, world_rank+1, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        }

        MPI_Allreduce(MPI_IN_PLACE, &endFlag, 1, MPI_INT, MPI_LAND, MPI_COMM_WORLD);
    
        if(!world_rank && iterations%1000==0) {
            FILE* file;

            char* file_name;

            asprintf(&file_name, "%d-%d.debug", world_size, sizeOfPlane);

            file = fopen(file_name, "a");
            fprintf(file, "Iterations: %lu\n", iterations);
            fclose(file);
        }
    } while(!endFlag);

    return iterations;
}

int main(int argc, char **argv)
{
    int sizeOfPlane = 10;
    double tolerance = 0.00001;
    double left = 4;
    double right = 2;
    double top = 1;
    double bottom = 3;
    bool debug = false;

    // 2D array that is worked on by each MPI process
    double** plane;

    // For MPI
    int world_rank, world_size;

    // For timing algorithm
    struct timespec start, end;

    // For counting how many iterations needed to smooth the plane
    unsigned long iterations;

    // For parsing flags
    int opt;

    while ((opt = getopt (argc, argv, "u:d:l:r:s:p:h:x")) != -1)
        switch (opt) {
            case 's':
                sizeOfPlane = (int) atoi(optarg);
                break;
            case 'x':
                debug = true;
                break;
            default:
                fprintf (stderr, "Unknown option character `\\x%x'.\n", optopt);
                return 1;
    }
 
    // Size of the plane must be at least 3x3
    if(sizeOfPlane < 3) {
        fprintf (stderr, "The size of the plane must be greater than 2\n");
        return 1;
    }

// -----------------------------------------------------------------------------
// Start of MPI Logic6
// -----------------------------------------------------------------------------

    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &world_rank);
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);

    int sizeOfInner = sizeOfPlane-2;
    int rowsPerThreadS = sizeOfInner/world_size+1;
    int rowsPerThreadE = sizeOfInner/world_size;
    int remainingRows = sizeOfInner - world_size * rowsPerThreadE;

    int numRows;

    if(world_rank < remainingRows) {
        numRows = rowsPerThreadS + 2;
    } else {
        numRows = rowsPerThreadE + 2;
    }

    plane = newSubPlane((unsigned int)sizeOfPlane, (unsigned int)numRows);

    populateSubPlane(plane, sizeOfPlane, numRows, top, bottom, left, right, world_rank, world_size);

    clock_gettime(CLOCK_MONOTONIC, &start);

    if(!world_rank && iterations%1000==0) {
        FILE* file;

        char* file_name;

        asprintf(&file_name, "%d-%d.debug", world_size, sizeOfPlane);

        file = fopen(file_name, "a");
        fprintf(file, "Starting relaxing\n", iterations);
        fclose(file);
    }

    iterations = relaxPlane(plane, numRows, sizeOfPlane, tolerance, world_rank, world_size);

    clock_gettime(CLOCK_MONOTONIC, &end);

    if(debug) {
        FILE* file;
        char* file_name;

        asprintf(&file_name, "%d-%d.result", world_size, sizeOfPlane);

        for(int i=0; i<world_size; i++) {
            if (i == world_rank) {
                file = fopen(file_name, world_rank == 0 ? "w" : "a");
                int startingRow = 1;
                int endingRow = numRows - 1;
                
                if(world_rank == 0) {
                    startingRow = 0;
                } else if(world_rank == world_size-1) {
                    endingRow = numRows;
                }
                
                for(int j=startingRow; j<endingRow; j++) {
                    for(int k=0; k<sizeOfPlane; k++) {
                        fprintf(file, "%f, ", plane[j][k]);
                    }
                    fprintf(file, "\n");
                }
                fclose(file);
            }
            MPI_Barrier(MPI_COMM_WORLD);
        }
        if(!world_rank) {
            file = fopen(file_name, "a");
            fprintf(file, "\nThreads: %d\n",world_size);
            fprintf(file, "Size of Pane: %d\n", sizeOfPlane);
            fprintf(file, "Iterations: %lu\n", iterations);
            fprintf(file, "Time: %Lfs\n", toSeconds(start, end));
            fclose(file);
        }
    }

    MPI_Finalize();

    if(!world_rank) {
        printf("Threads: %d\n",world_size);
        printf("Size of Pane: %d\n", sizeOfPlane);
        printf("Iterations: %lu\n", iterations);
        printf("Time: %Lfs\n", toSeconds(start, end));
    }

    return 0;
}