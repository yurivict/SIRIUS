extern "C" void unpack_z_cols_gpu(cuDoubleComplex* z_cols_packed__,
                                  cuDoubleComplex* fft_buf__,
                                  int size_x__,
                                  int size_y__,
                                  int size_z__,
                                  int num_z_cols__,
                                  int const* z_columns_pos__,
                                  int stream_id__);

extern "C" void pack_z_cols_gpu(cuDoubleComplex* z_cols_packed__,
                                cuDoubleComplex* fft_buf__,
                                int size_x__,
                                int size_y__,
                                int size_z__,
                                int num_z_cols__,
                                int const* z_columns_pos__,
                                int stream_id__);
template <int direction>
void FFT3D::transform_xy_serial(Gvec const& gvec__)
{
    int size_xy = fft_grid_.size(0) * fft_grid_.size(1);

    if (pu_ == CPU)
    {
        #pragma omp parallel num_threads(num_fft_workers_)
        {
            int tid = omp_get_thread_num();
            #pragma omp for
            for (int z = 0; z < fft_grid_.size(2); z++)
            {
                /* copy xy plane into local buffer */
                std::memcpy(fftw_buffer_xy_[tid], &fftw_buffer_[z * size_xy], sizeof(double_complex) * size_xy);
                /* execute 2D FFT */
                switch (direction)
                {
                    case 1:
                    {
                        fftw_execute(plan_backward_xy_[tid]);
                        break;
                    }
                    case -1:
                    {
                        fftw_execute(plan_forward_xy_[tid]);
                        break;
                    }
                    default:
                    {
                        TERMINATE("wrong direction");
                    }
                }
                /* copy result to the main FFT buffer */
                std::memcpy(&fftw_buffer_[z * size_xy], fftw_buffer_xy_[tid], sizeof(double_complex) * size_xy);
            }
        }
    }
    #ifdef __GPU
    if (pu_ == GPU)
    {
        /* stream #0 will be doing cuFFT */
        switch (direction)
        {
            case 1:
            {
                //double_complex z = mdarray<double_complex, 1>(&fft_buffer_aux_[0], cufft_nbatch_ * gvec__.z_columns().size()).checksum();
                //printf("checksum(fft_buffer_aux): %f %f\n", z.real(), z.imag());

                /* srteam #0 copies packed columns to GPU */
                acc::copyin(fft_buffer_aux_.at<GPU>(), fft_buffer_aux_.at<CPU>(), cufft_nbatch_ * gvec__.z_columns().size(), 0);
                /* srteam #0 unpacks z-columns into proper position of FFT buffer */
                unpack_z_cols_gpu(fft_buffer_aux_.at<GPU>(), cufft_buf_.at<GPU>(), fft_grid_.size(0), fft_grid_.size(1), 
                                  cufft_nbatch_, (int)gvec__.z_columns().size(), gvec__.z_columns_pos().at<GPU>(), 0);
                /* stream #0 executes FFT */
                cufft_backward_transform(cufft_plan_xy_, cufft_buf_.at<GPU>());
                break;
            }
            case -1:
            {
                /* stream #1 copies part of FFT buffer to CPU */
                acc::copyout(cufft_buf_.at<CPU>(cufft_nbatch_ * size_xy), cufft_buf_.at<GPU>(cufft_nbatch_ * size_xy),
                             size_xy * (local_size_z_ - cufft_nbatch_), 1);
                /* stream #0 executes FFT */
                cufft_forward_transform(cufft_plan_xy_, cufft_buf_.at<GPU>());
                fft_buffer_aux_.zero_on_device();
                /* stream #0 packs z-columns */
                pack_z_cols_gpu(fft_buffer_aux_.at<GPU>(), cufft_buf_.at<GPU>(), fft_grid_.size(0), fft_grid_.size(1), 
                                cufft_nbatch_, (int)gvec__.z_columns().size(), gvec__.z_columns_pos().at<GPU>(), 0);

                /* srteam #0 copies packed columns to CPU */
                acc::copyout(fft_buffer_aux_.at<CPU>(), fft_buffer_aux_.at<GPU>(), cufft_nbatch_ * gvec__.z_columns().size(), 0);
                /* stream #1 waits to complete memory copy */
                cuda_stream_synchronize(1);
                //double_complex z = mdarray<double_complex, 1>(&fft_buffer_aux_[0], cufft_nbatch_ * gvec__.z_columns().size()).checksum();
                //printf("checksum(fft_buffer_aux): %f %f\n", z.real(), z.imag());
                break;
            }
        }
        
        /* CPU starts working on remaining part of FFT buffer */
        #pragma omp parallel num_threads(num_fft_workers_)
        {
            int tid = omp_get_thread_num();
            #pragma omp for
            for (int z = cufft_nbatch_; z < local_size_z_; z++)
            {
                /* copy xy plane into local buffer */
                std::memcpy(fftw_buffer_xy_[tid], &fftw_buffer_[z * size_xy], sizeof(double_complex) * size_xy);
                /* execute 2D FFT */
                switch (direction)
                {
                    case 1:
                    {
                        fftw_execute(plan_backward_xy_[tid]);
                        break;
                    }
                    case -1:
                    {
                        fftw_execute(plan_forward_xy_[tid]);
                        break;
                    }
                    default:
                    {
                        TERMINATE("wrong direction");
                    }
                }
                /* copy result to the main FFT buffer */
                std::memcpy(&fftw_buffer_[z * size_xy], fftw_buffer_xy_[tid], sizeof(double_complex) * size_xy);
            }
        }
        if (direction == 1)
        {
            /* stream #1 copies data to GPU */
            acc::copyin(cufft_buf_.at<GPU>(cufft_nbatch_ * size_xy), cufft_buf_.at<CPU>(cufft_nbatch_ * size_xy),
                        size_xy * (local_size_z_ - cufft_nbatch_), 1);
            cuda_stream_synchronize(1);
        }
        /* wait for stram #0 */
        cuda_stream_synchronize(0);
    }
    #endif
}

template <int direction>
void FFT3D::transform_xy_parallel(Gvec const& gvec__)
{
    int size_xy = fft_grid_.size(0) * fft_grid_.size(1);
    if (pu_ == CPU)
    {
        #pragma omp parallel num_threads(num_fft_workers_)
        {
            int tid = omp_get_thread_num();
            #pragma omp for
            for (int iz = 0; iz < local_size_z_; iz++)
            {
                switch (direction)
                {
                    case 1:
                    {
                        /* clear xy-buffer */
                        std::memset(fftw_buffer_xy_[tid], 0, sizeof(double_complex) * size_xy);
                        /* load z-columns into proper location */
                        for (size_t i = 0; i < gvec__.z_columns().size(); i++)
                        {
                            int x = gvec__.z_columns()[i].x;
                            int y = gvec__.z_columns()[i].y;
                            if (x < 0) x += fft_grid_.size(0);
                            if (y < 0) y += fft_grid_.size(1);

                            fftw_buffer_xy_[tid][x + y * fft_grid_.size(0)] = fft_buffer_aux_(iz + local_size_z_ * i);
                        }
                        /* execute local FFT transform */
                        fftw_execute(plan_backward_xy_[tid]);
                        /* copy xy plane to the fft buffer */
                        std::memcpy(&fftw_buffer_[iz * size_xy], fftw_buffer_xy_[tid], sizeof(fftw_complex) * size_xy);
                        break;
                    }
                    case -1:
                    {
                        /* copy xy plane from the fft buffer */
                        std::memcpy(fftw_buffer_xy_[tid], &fftw_buffer_[iz * size_xy], sizeof(fftw_complex) * size_xy);
                        /* execute local FFT transform */
                        fftw_execute(plan_forward_xy_[tid]);
                        /* get z-columns */
                        for (size_t i = 0; i < gvec__.z_columns().size(); i++)
                        {
                            int x = gvec__.z_columns()[i].x;
                            int y = gvec__.z_columns()[i].y;
                            if (x < 0) x += fft_grid_.size(0);
                            if (y < 0) y += fft_grid_.size(1);

                            fft_buffer_aux_(iz + local_size_z_ * i) = fftw_buffer_xy_[tid][x + y * fft_grid_.size(0)];
                        }
                        break;
                    }
                    default:
                    {
                        TERMINATE("wrong direction");
                    }
                }
            }
        }
    }
    //#ifdef __GPU
    //if (pu_ == GPU)
    //{
    //    if (direction == 1)
    //    {
    //        memset(fftw_buffer_, 0, sizeof(double_complex) * local_size());
    //        #pragma omp parallel for num_threads(num_fft_workers_)
    //        for (int iz = 0; iz < local_size_z_; iz++)
    //        {
    //            for (int n = 0; n < (int)z_sticks_coord__.size(); n++)
    //            {
    //                int x = z_sticks_coord__[n].first;
    //                int y = z_sticks_coord__[n].second;
    //                fftw_buffer_[x + y * size(0) + iz * size_xy] = fft_buffer_aux_(iz + local_size_z_ * n);
    //            }
    //        }
    //        cufft_buf_.copy_to_device();
    //        cufft_backward_transform(cufft_plan_xy_, cufft_buf_.at<GPU>());
    //        cufft_buf_.copy_to_host();
    //    }
    //    if (direction == -1)
    //    {
    //        cufft_buf_.copy_to_device();
    //        cufft_forward_transform(cufft_plan_xy_, cufft_buf_.at<GPU>());
    //        cufft_buf_.copy_to_host();
    //        #pragma omp parallel for num_threads(num_fft_workers_)
    //        for (int iz = 0; iz < local_size_z_; iz++)
    //        {
    //            for (int n = 0; n < (int)z_sticks_coord__.size(); n++)
    //            {
    //                int x = z_sticks_coord__[n].first;
    //                int y = z_sticks_coord__[n].second;
    //                fft_buffer_aux_(iz + local_size_z_ * n) = fftw_buffer_[x + y * size(0) + iz * size_xy];
    //            }
    //        }
    //    }
    //}
    //#endif
}

template <int direction, bool use_reduction>
void FFT3D::transform_z_serial(Gvec const& gvec__, double_complex* data__)
{
    int size_xy = fft_grid_.size(0) * fft_grid_.size(1);
    double norm = 1.0 / size();
    #pragma omp parallel num_threads(num_fft_workers_)
    {
        int tid = omp_get_thread_num();
        #pragma omp for schedule(dynamic, 1)
        for (size_t i = 0; i < gvec__.z_columns().size(); i++)
        {
            /* x,y coordinates of G-vectors */
            int x = gvec__.z_columns()[i].x;
            int y = gvec__.z_columns()[i].y;
            /* coordinates inside FFT grid */
            if (x < 0) x += fft_grid_.size(0);
            if (y < 0) y += fft_grid_.size(1);

            int offset = gvec__.z_columns()[i].offset;

            switch (direction)
            {
                case 1:
                {
                    //std::fill(fftw_buffer_z_[tid], fftw_buffer_z_[tid] + size(2), double_complex(0, 0));
                    std::memset(fftw_buffer_z_[tid], 0, fft_grid_.size(2) * sizeof(double_complex));
                    /* load column into local FFT buffer */
                    for (size_t j = 0; j < gvec__.z_columns()[i].z.size(); j++)
                    {
                        /* z-coordinate of G-vector */
                        int z = gvec__.z_columns()[i].z[j];
                        /* coordinate inside FFT grid */
                        if (z < 0) z += fft_grid_.size(2);
                        fftw_buffer_z_[tid][z] = data__[offset + j];
                    }
                    /* column with {x,y} = {0,0} has only non-negative z components */
                    if (use_reduction && !gvec__.z_columns()[i].x && !gvec__.z_columns()[i].y)
                    {
                        for (size_t j = 0; j < gvec__.z_columns()[i].z.size(); j++)
                        {
                            int z = -gvec__.z_columns()[i].z[j];
                            if (z < 0) z += fft_grid_.size(2);
                            fftw_buffer_z_[tid][z] = std::conj(data__[offset + j]);
                        }
                    }
                    /* execute 1D transform of a z-column */
                    fftw_execute(plan_backward_z_[tid]);
                    /* load z-column into main FFT buffer */
                    for (int z = 0; z < fft_grid_.size(2); z++)
                    {
                        fftw_buffer_[x + y * fft_grid_.size(0) + z * size_xy] = fftw_buffer_z_[tid][z];
                    }
                    for (int z = 0; z < cufft_nbatch_; z++)
                    {
                        fft_buffer_aux_[z + i * cufft_nbatch_] = fftw_buffer_z_[tid][z];
                    }

                    if (use_reduction && (gvec__.z_columns()[i].x || gvec__.z_columns()[i].y))
                    {
                        /* x,y coordinates of inverse G-vectors */
                        int x = -gvec__.z_columns()[i].x;
                        int y = -gvec__.z_columns()[i].y;
                        /* coordinates inside FFT grid */
                        if (x < 0) x += fft_grid_.size(0);
                        if (y < 0) y += fft_grid_.size(1);
                        /* load conjugated z-column into main FFT buffer */
                        for (int z = 0; z < fft_grid_.size(2); z++)
                        {
                            fftw_buffer_[x + y * fft_grid_.size(0) + z * size_xy] = std::conj(fftw_buffer_z_[tid][z]);
                        }
                    }
                    break;
                }
                case -1:
                {
                    for (int z = 0; z < fft_grid_.size(2); z++)
                    {
                        fftw_buffer_z_[tid][z] = fftw_buffer_[x + y * fft_grid_.size(0) + z * size_xy];
                    }
                    for (int z = 0; z < cufft_nbatch_; z++)
                    {
                        fftw_buffer_z_[tid][z] = fft_buffer_aux_[z + i * cufft_nbatch_];
                    }
                    fftw_execute(plan_forward_z_[tid]);
                    for (size_t j = 0; j < gvec__.z_columns()[i].z.size(); j++)
                    {
                        int z = gvec__.z_columns()[i].z[j];
                        if (z < 0) z += fft_grid_.size(2);
                        data__[offset + j] = fftw_buffer_z_[tid][z] * norm;
                    }
                    break;
                }
                default:
                {
                    TERMINATE("wrong direction");
                }
            }
        }
    }
}

template <int direction>
void FFT3D::transform_z_parallel(Gvec const& gvec__, double_complex* data__)
{
    int rank = comm_.rank();
    int num_zcol_local = gvec__.zcol_fft_distr().counts[rank];
    double norm = 1.0 / size();

    if (direction == -1)
    {
        block_data_descriptor send(comm_.size());
        block_data_descriptor recv(comm_.size());
        for (int r = 0; r < comm_.size(); r++)
        {
            send.counts[r] = static_cast<int>(spl_z_.local_size(rank) * gvec__.zcol_fft_distr().counts[r]);
            recv.counts[r] = static_cast<int>(spl_z_.local_size(r) * gvec__.zcol_fft_distr().counts[rank]);
        }
        send.calc_offsets();
        recv.calc_offsets();
        
        std::memcpy(fftw_buffer_, &fft_buffer_aux_(0), gvec__.z_columns().size() * local_size_z_ * sizeof(double_complex)); 

        comm_.alltoall(fftw_buffer_, &send.counts[0], &send.offsets[0], &fft_buffer_aux_(0), &recv.counts[0], &recv.offsets[0]);
    }

    #pragma omp parallel num_threads(num_fft_workers_)
    {
        int tid = omp_get_thread_num();
        #pragma omp for schedule(dynamic, 1)
        for (int i = 0; i < num_zcol_local; i++)
        {
            int icol = gvec__.zcol_fft_distr().offsets[rank] + i;
            int offset = gvec__.z_columns()[icol].offset;

            switch (direction)
            {
                case 1:
                {
                    /* clear z buffer */
                    //std::fill(fftw_buffer_z_[tid], fftw_buffer_z_[tid] + size(2), double_complex(0, 0));
                    std::memset(fftw_buffer_z_[tid], 0, fft_grid_.size(2) * sizeof(double_complex));
                    /* load z column into buffer */
                    for (size_t j = 0; j < gvec__.z_columns()[icol].z.size(); j++)
                    {
                        int z = gvec__.z_columns()[icol].z[j];
                        if (z < 0) z += fft_grid_.size(2);

                        fftw_buffer_z_[tid][z] = data__[offset + j];
                    }
                    /* perform local FFT transform of a column */
                    fftw_execute(plan_backward_z_[tid]);
                    /* redistribute z-column for a forthcoming all-to-all */ 
                    for (int r = 0; r < comm_.size(); r++)
                    {
                        int lsz = (int)spl_z_.local_size(r);
                        size_t offs = spl_z_.global_offset(r);
                    
                        std::memcpy(&fft_buffer_aux_(offs * num_zcol_local + i * lsz), &fftw_buffer_z_[tid][offs], 
                                    lsz * sizeof(double_complex));
                    }
                    break;

                }
                case -1:
                {
                    /* collect full z-column */ 
                    for (int r = 0; r < comm_.size(); r++)
                    {
                        int lsz = (int)spl_z_.local_size(r);
                        size_t offs = spl_z_.global_offset(r);
                    
                        std::memcpy(&fftw_buffer_z_[tid][offs], &fft_buffer_aux_(offs * num_zcol_local + i * lsz),
                                    lsz * sizeof(double_complex));
                    }
                    /* perform local FFT transform of a column */
                    fftw_execute(plan_forward_z_[tid]);
                    /* save z column of PW coefficients*/
                    for (size_t j = 0; j < gvec__.z_columns()[icol].z.size(); j++)
                    {
                        int z = gvec__.z_columns()[icol].z[j];
                        if (z < 0) z += fft_grid_.size(2);

                        data__[offset + j] = fftw_buffer_z_[tid][z] * norm;
                    }
                    break;

                }
                default:
                {
                    TERMINATE("wrong direction");
                }
            }
        }
    }

    /* scatter z-columns between slabs of FFT buffer */
    if (direction == 1)
    {
        block_data_descriptor send(comm_.size());
        block_data_descriptor recv(comm_.size());
        for (int r = 0; r < comm_.size(); r++)
        {
            send.counts[r] = static_cast<int>(spl_z_.local_size(r) * gvec__.zcol_fft_distr().counts[rank]);
            recv.counts[r] = static_cast<int>(spl_z_.local_size(rank) * gvec__.zcol_fft_distr().counts[r]);
        }
        send.calc_offsets();
        recv.calc_offsets();
        
        comm_.alltoall(&fft_buffer_aux_(0), &send.counts[0], &send.offsets[0], fftw_buffer_, &recv.counts[0], &recv.offsets[0]);

        std::memcpy(&fft_buffer_aux_(0), fftw_buffer_, gvec__.z_columns().size() * local_size_z_ * sizeof(double_complex)); 
    }
}


template <int direction>
void FFT3D::transform(Gvec const& gvec__, double_complex* data__)
{
    /* reallocate auxiliary buffer if needed */
    size_t sz_max;
    if (comm_.size() > 1)
    {
        int rank = comm_.rank();
        int num_zcol_local = gvec__.zcol_fft_distr().counts[rank];
        /* we need this buffer for mpi_alltoall */
        sz_max = std::max(fft_grid_.size(2) * num_zcol_local, local_size());
    }
    else
    {
        sz_max = fft_grid_.size(2) * gvec__.z_columns().size();
    }
    if (sz_max > fft_buffer_aux_.size())
    {
        fft_buffer_aux_ = mdarray<double_complex, 1>(sz_max);
        fft_buffer_aux_.pin_memory();
        fft_buffer_aux_.allocate_on_device();
    }

    /* single node FFT */
    if (comm_.size() == 1)
    {
        if (pu_ == CPU || !cufft3d_)
        {
            switch (direction)
            {
                case 1:
                {
                    std::memset(fftw_buffer_, 0, size() * sizeof(double_complex));
                    if (gvec__.reduced())
                    {
                        transform_z_serial<1, true>(gvec__, data__);
                    }
                    else
                    {
                        transform_z_serial<1, false>(gvec__, data__);
                    }
                    transform_xy_serial<1>(gvec__);
                    break;
                }
                case -1:
                {
                    transform_xy_serial<-1>(gvec__);
                    transform_z_serial<-1, false>(gvec__, data__);
                    break;
                }
                default:
                {
                    TERMINATE("wrong direction");
                }
            }
        }
        #ifdef __GPU
        if (pu_ == GPU && cufft3d_)
        {
            switch (direction)
            {
                case 1:
                {
                    cufft_backward_transform(cufft_plan_, cufft_buf_.at<GPU>());
                    break;
                }
                case -1:
                {
                    cufft_forward_transform(cufft_plan_, cufft_buf_.at<GPU>());
                    break;
                }
            }
        }
        #endif
    }
    else
    {
        switch (direction)
        {
            case 1:
            {
                transform_z_parallel<1>(gvec__, data__);
                transform_xy_parallel<1>(gvec__);
                break;
            }
            case -1:
            {
                transform_xy_parallel<-1>(gvec__);
                transform_z_parallel<-1>(gvec__, data__);
                break;
            }
            default:
            {
                TERMINATE("wrong direction");
            }
        }   
    }
}

        
