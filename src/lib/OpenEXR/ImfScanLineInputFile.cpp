//
// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Contributors to the OpenEXR Project.
//

//-----------------------------------------------------------------------------
//
//	class ScanLineInputFile
//
//-----------------------------------------------------------------------------

#include "ImfScanLineInputFile.h"

#include "Iex.h"

#include "IlmThreadPool.h"
#if ILMTHREAD_THREADING_ENABLED
#    include "IlmThreadSemaphore.h"
#endif

#include "ImfFrameBuffer.h"
#include "ImfInputPartData.h"

#include <atomic>
#include <mutex>
#include <vector>

OPENEXR_IMF_INTERNAL_NAMESPACE_SOURCE_ENTER

struct ScanLineProcess
{
    ~ScanLineProcess ()
    {
        if (!first)
            exr_decoding_destroy (decoder.context, &decoder);
    }

    void run_decode (
        exr_const_context_t ctxt,
        int pn,
        const FrameBuffer *outfb,
        int fbY,
        int fbLastY,
        const std::vector<Slice> &filllist);

    void run_unpack (
        exr_const_context_t ctxt,
        int pn,
        const FrameBuffer *outfb,
        int fbY,
        int fbLastY,
        const std::vector<Slice> &filllist);

    void update_pointers (
        const FrameBuffer *outfb,
        int fbY,
        int fbLastY);

    void run_fill (
        const FrameBuffer *outfb,
        int fbY,
        const std::vector<Slice> &filllist);

    exr_result_t          last_decode_err = EXR_ERR_UNKNOWN;
    bool                  first = true;
    exr_chunk_info_t      cinfo;
    exr_decode_pipeline_t decoder;

    std::shared_ptr<ScanLineProcess> next;
};

struct ScanLineInputFile::Data
{
    Data (Context *ctxt, int pN, int nT)
    : _ctxt (ctxt)
    , partNumber (pN)
    , numThreads (nT)
#if ILMTHREAD_THREADING_ENABLED
    , _sem ((unsigned int)nT)
#endif
    {}

    Context* _ctxt;
    int partNumber;
    int numThreads;
    Header header;
    bool header_filled = false;

    exr_storage_t _storage = EXR_STORAGE_UNKNOWN;

    // TODO: remove once we can remove deprecated API
    std::vector<char> _pixel_data_scratch;

    void readPixels (const FrameBuffer &fb, int scanLine1, int scanLine2);

    FrameBuffer frameBuffer;
    std::vector<Slice> fill_list;

    std::vector<std::string> _failures;

#if ILMTHREAD_THREADING_ENABLED
    std::mutex _mx;
    ILMTHREAD_NAMESPACE::Semaphore _sem;
#endif

    std::shared_ptr<ScanLineProcess> processStack;
    std::shared_ptr<ScanLineProcess> getChunkProcess ()
    {
        std::shared_ptr<ScanLineProcess> retval;
#if ILMTHREAD_THREADING_ENABLED
        std::lock_guard<std::mutex> lk (_mx);
#endif
        retval = processStack;
        if (!retval)
            retval = std::make_shared<ScanLineProcess> ();
        processStack = retval->next;
        retval->next.reset();
        return retval;
    }
    void putChunkProcess (std::shared_ptr<ScanLineProcess> sp)
    {
#if ILMTHREAD_THREADING_ENABLED
        std::lock_guard<std::mutex> lk (_mx);
#endif
        sp->next = processStack;
        processStack = sp;
    }

#if ILMTHREAD_THREADING_ENABLED
    class LineBufferTask final : public ILMTHREAD_NAMESPACE::Task
    {
    public:
        LineBufferTask (
            ILMTHREAD_NAMESPACE::TaskGroup* group,
            Data*                   ifd,
            const FrameBuffer*      outfb,
            const exr_chunk_info_t& cinfo,
            int                     fby,
            int                     endScan)
            : Task (group)
            , _outfb (outfb)
            , _ifd (ifd)
            , _fby (fby)
            , _last_fby (endScan)
            , _line (ifd->getChunkProcess ())
        {
            _line->cinfo = cinfo;
        }

        ~LineBufferTask () override
        {
            if (_line)
                _ifd->putChunkProcess (std::move (_line));
            _ifd->_sem.post ();
        }

        void execute () override;

    private:
        void run_decode ();

        const FrameBuffer* _outfb;
        Data*              _ifd;
        int                _fby;
        int                _last_fby;

        std::shared_ptr<ScanLineProcess> _line;
    };
#endif
};

ScanLineInputFile::ScanLineInputFile (InputPartData* part)
    : _ctxt (part->context),
      _data (std::make_shared<Data> (&_ctxt, part->partNumber, part->numThreads))
{
    _data->_storage = _ctxt.storage (_data->partNumber);
}

ScanLineInputFile::ScanLineInputFile (
    const char*               filename,
    const ContextInitializer& ctxtinit,
    int                       numThreads)
    : _data (std::make_shared<Data> (&_ctxt, 0, numThreads))
{
    _ctxt.startRead (filename, ctxtinit);
    _data->_storage = _ctxt.storage (_data->partNumber);
}

ScanLineInputFile::ScanLineInputFile (
        OPENEXR_IMF_INTERNAL_NAMESPACE::IStream& is,
        int numThreads)
    : ScanLineInputFile (
        is.fileName (),
        ContextInitializer ()
        .silentHeaderParse (true)
        .strictHeaderValidation (false)
        .setInputStream (&is),
        numThreads)
{
}

ScanLineInputFile::ScanLineInputFile (const char filename[], int numThreads)
    : ScanLineInputFile (
        filename,
        ContextInitializer ()
        .silentHeaderParse (true)
        .strictHeaderValidation (false),
        numThreads)
{
}

const char*
ScanLineInputFile::fileName () const
{
    return _ctxt.fileName ();
}

const Header&
ScanLineInputFile::header () const
{
#if ILMTHREAD_THREADING_ENABLED
    std::lock_guard<std::mutex> lock (_data->_mx);
#endif
    if (!_data->header_filled)
    {
        _data->header = _ctxt.header (_data->partNumber);
        _data->header_filled = true;
    }
    return _data->header;
}

int
ScanLineInputFile::version () const
{
    return _ctxt.version ();
}

void
ScanLineInputFile::setFrameBuffer (const FrameBuffer& frameBuffer)
{
#if ILMTHREAD_THREADING_ENABLED
    std::lock_guard<std::mutex> lock (_data->_mx);
#endif
    _data->fill_list.clear ();

    for (FrameBuffer::ConstIterator j = frameBuffer.begin ();
         j != frameBuffer.end ();
         ++j)
    {
        const exr_attr_chlist_entry_t* curc = _ctxt.findChannel (
            _data->partNumber, j.name ());

        if (!curc)
        {
            _data->fill_list.push_back (j.slice ());
            continue;
        }

        if (curc->x_sampling != j.slice ().xSampling ||
            curc->y_sampling != j.slice ().ySampling)
            THROW (
                IEX_NAMESPACE::ArgExc,
                "X and/or y subsampling factors "
                "of \""
                    << j.name ()
                    << "\" channel "
                       "of input file \""
                    << fileName ()
                    << "\" are "
                       "not compatible with the frame buffer's "
                       "subsampling factors.");
    }

    _data->frameBuffer = frameBuffer;
    _data->processStack.reset();
}

const FrameBuffer&
ScanLineInputFile::frameBuffer () const
{
#if ILMTHREAD_THREADING_ENABLED
    std::lock_guard<std::mutex> lock (_data->_mx);
#endif
    return _data->frameBuffer;
}

bool
ScanLineInputFile::isComplete () const
{
    return _ctxt.chunkTableValid (_data->partNumber);
}

bool
ScanLineInputFile::isOptimizationEnabled () const
{
    // TODO: the core library has a number of special cased patterns,
    // this is all kind of ... not useful? for now, return a pattern
    // similar to legacy version
    return _ctxt.channels (_data->partNumber)->num_channels != 2;
}

void
ScanLineInputFile::readPixels (int scanLine1, int scanLine2)
{
    _data->readPixels (frameBuffer (), scanLine1, scanLine2);
}

////////////////////////////////////////

void
ScanLineInputFile::readPixels (int scanLine)
{
    readPixels (scanLine, scanLine);
}

////////////////////////////////////////

void
ScanLineInputFile::rawPixelData (
    int firstScanLine, const char*& pixelData, int& pixelDataSize)
{
    uint64_t maxsize = 0;
    if (EXR_ERR_SUCCESS !=
        exr_get_chunk_unpacked_size (_ctxt, _data->partNumber, &maxsize))
    {
        THROW (
            IEX_NAMESPACE::ArgExc,
            "Unable to query data size of chunk in file '" << fileName ()
                                                           << "'");
    }

    // again, doesn't actually provide any safety given we're handing
    // back a pointer... but will at least prevent two threads
    // allocating at the same time and getting sliced
#if ILMTHREAD_THREADING_ENABLED
    std::lock_guard<std::mutex> lock (_data->_mx);
#endif
    _data->_pixel_data_scratch.resize (maxsize);

    pixelData     = _data->_pixel_data_scratch.data ();
    pixelDataSize = static_cast<int> (maxsize);

    rawPixelDataToBuffer (
        firstScanLine, _data->_pixel_data_scratch.data (), pixelDataSize);
}

void
ScanLineInputFile::rawPixelDataToBuffer (
    int scanLine, char* pixelData, int& pixelDataSize) const
{
    exr_chunk_info_t cinfo;
    if (EXR_ERR_SUCCESS == exr_read_scanline_chunk_info (
                               _ctxt, _data->partNumber, scanLine, &cinfo))
    {
        if (cinfo.packed_size > static_cast<uint64_t> (pixelDataSize))
        {
            THROW (
                IEX_NAMESPACE::ArgExc,
                "Error reading pixel data from image "
                "file \""
                    << fileName ()
                    << "\". Provided buffer is too small to read raw pixel data:"
                    << pixelDataSize << " bytes.");
        }

        pixelDataSize = static_cast<int> (cinfo.packed_size);

        if (EXR_ERR_SUCCESS !=
            exr_read_chunk (_ctxt, _data->partNumber, &cinfo, pixelData))
        {
            THROW (
                IEX_NAMESPACE::ArgExc,
                "Error reading pixel data from image "
                "file \""
                    << fileName () << "\". Unable to read raw pixel data of "
                    << pixelDataSize << " bytes.");
        }
    }
    else
    {
        if (_data->_storage == EXR_STORAGE_TILED)
        {
            THROW (
                IEX_NAMESPACE::ArgExc,
                "Error reading pixel data from image "
                "file \""
                    << fileName ()
                    << "\". Tried to read a raw scanline from a tiled image.");
        }
        else
        {
            THROW (
                IEX_NAMESPACE::ArgExc,
                "Error reading pixel data from image "
                "file \""
                    << fileName ()
                    << "\". Unable to query data block information.");
        }
    }
}

////////////////////////////////////////

void ScanLineInputFile::Data::readPixels (
    const FrameBuffer &fb, int scanLine1, int scanLine2)
{
    exr_attr_box2i_t dw = _ctxt->dataWindow (partNumber);
    int32_t scansperchunk = 1;
    int64_t nchunks;

    if (EXR_ERR_SUCCESS != exr_get_scanlines_per_chunk (*_ctxt, partNumber, &scansperchunk))
    {
        THROW (
            IEX_NAMESPACE::ArgExc,
            "Error querying scanline counts from image "
            "file \"" << _ctxt->fileName () << "\".");
    }

    if (scanLine2 < scanLine1)
        std::swap (scanLine1, scanLine2);

    if (scanLine1 < dw.min.y || scanLine2 > dw.max.y)
    {
        THROW (
            IEX_NAMESPACE::ArgExc,
            "Tried to read scan line outside "
            "the image file's data window: "
            << scanLine1 << " - " << scanLine2
            << " vs datawindow "
            << dw.min.y << " - " << dw.max.y);
    }

    nchunks = 1 + (((int64_t) scanLine2 - (int64_t) scanLine1) / scansperchunk);

    exr_chunk_info_t      cinfo;
#if ILMTHREAD_THREADING_ENABLED
    if (nchunks > 1 && numThreads > 1)
    {
        ILMTHREAD_NAMESPACE::TaskGroup tg;

        for (int y = scanLine1; y <= scanLine2; )
        {
            if (EXR_ERR_SUCCESS != exr_read_scanline_chunk_info (*_ctxt, partNumber, y, &cinfo))
                throw IEX_NAMESPACE::InputExc ("Unable to query scanline information");

            // used for honoring the numThreads
            _sem.wait ();

            ILMTHREAD_NAMESPACE::ThreadPool::addGlobalTask (
                new LineBufferTask (&tg, this, &fb, cinfo, y, scanLine2) );

            y += scansperchunk - (y - cinfo.start_y);
        }
    }
    else
#endif
    {
        auto sp = getChunkProcess ();

        for (int y = scanLine1; y <= scanLine2; )
        {
            if (EXR_ERR_SUCCESS != exr_read_scanline_chunk_info (*_ctxt, partNumber, y, &cinfo))
                throw IEX_NAMESPACE::InputExc ("Unable to query scanline information");

            // do we have the same chunk where we can just re-run the unpack
            // (i.e. people reading 1 scan at a time in a multi-scanline chunk)
            if (!sp->first && sp->cinfo.idx == cinfo.idx &&
                sp->last_decode_err == EXR_ERR_SUCCESS)
            {
                sp->run_unpack (
                    *_ctxt,
                    partNumber,
                    &fb,
                    y,
                    scanLine2,
                    fill_list);
            }
            else
            {
                sp->cinfo = cinfo;
                sp->run_decode (
                    *_ctxt,
                    partNumber,
                    &fb,
                    y,
                    scanLine2,
                    fill_list);
            }

            y += scansperchunk - (y - cinfo.start_y);
        }

        putChunkProcess (std::move(sp));
    }

    if (! _failures.empty())
    {
        std::string fail = _failures[0];
        _failures.clear ();
        throw IEX_NAMESPACE::IoExc (fail);
    }
}

////////////////////////////////////////

#if ILMTHREAD_THREADING_ENABLED
void ScanLineInputFile::Data::LineBufferTask::execute ()
{
    try
    {
        _line->run_decode (
            *(_ifd->_ctxt),
            _ifd->partNumber,
            _outfb,
            _fby,
            _last_fby,
            _ifd->fill_list);
    }
    catch (std::exception &e)
    {
        std::lock_guard<std::mutex> lock (_ifd->_mx);
        _ifd->_failures.emplace_back (std::string (e.what()));
    }
}
#endif

////////////////////////////////////////

void ScanLineProcess::run_decode (
    exr_const_context_t ctxt,
    int pn,
    const FrameBuffer *outfb,
    int fbY,
    int fbLastY,
    const std::vector<Slice> &filllist)
{
    last_decode_err = EXR_ERR_UNKNOWN;
    // stash the flag off to make sure to clean up in the event
    // of an exception by changing the flag after init...
    bool isfirst = first;
    if (first)
    {
        if (EXR_ERR_SUCCESS !=
            exr_decoding_initialize (ctxt, pn, &cinfo, &decoder))
        {
            throw IEX_NAMESPACE::IoExc ("Unable to initialize decode pipeline");
        }

        first = false;
    }
    else
    {
        if (EXR_ERR_SUCCESS !=
            exr_decoding_update (ctxt, pn, &cinfo, &decoder))
        {
            throw IEX_NAMESPACE::IoExc ("Unable to update decode pipeline");
        }
    }

    update_pointers (outfb, fbY, fbLastY);

    if (isfirst)
    {
        if (EXR_ERR_SUCCESS !=
            exr_decoding_choose_default_routines (ctxt, pn, &decoder))
        {
            throw IEX_NAMESPACE::IoExc ("Unable to choose decoder routines");
        }
    }

    last_decode_err = exr_decoding_run (ctxt, pn, &decoder);
    if (EXR_ERR_SUCCESS != last_decode_err)
        throw IEX_NAMESPACE::IoExc ("Unable to run decoder");

    run_fill (outfb, fbY, filllist);
}

////////////////////////////////////////

void ScanLineProcess::run_unpack (
    exr_const_context_t ctxt,
    int pn,
    const FrameBuffer *outfb,
    int fbY,
    int fbLastY,
    const std::vector<Slice> &filllist)
{
    update_pointers (outfb, fbY, fbLastY);

    /* won't work for deep where we need to re-allocate the number of
     * samples but for normal scanlines is fine to just bypass pipe
     * and run the unpacker */
    if (decoder.chunk.unpacked_size > 0 && decoder.unpack_and_convert_fn)
    {
        last_decode_err = decoder.unpack_and_convert_fn (&decoder);
        if (EXR_ERR_SUCCESS != last_decode_err)
            throw IEX_NAMESPACE::IoExc ("Unable to run decoder");
    }

    run_fill (outfb, fbY, filllist);
}

////////////////////////////////////////

void ScanLineProcess::update_pointers (
    const FrameBuffer *outfb, int fbY, int fbLastY)
{
    decoder.user_line_begin_skip = fbY - cinfo.start_y;
    decoder.user_line_end_ignore = 0;
    int64_t endY = (int64_t)cinfo.start_y + (int64_t)cinfo.height - 1;
    if ((int64_t)fbLastY < endY)
        decoder.user_line_end_ignore = (int32_t)(endY - fbLastY);

    for (int c = 0; c < decoder.channel_count; ++c)
    {
        exr_coding_channel_info_t& curchan = decoder.channels[c];
        uint8_t*                   ptr;
        const Slice*               fbslice;

        fbslice = outfb->findSlice (curchan.channel_name);

        if (curchan.height == 0 || !fbslice)
        {
            curchan.decode_to_ptr     = NULL;
            curchan.user_pixel_stride = 0;
            curchan.user_line_stride  = 0;
            continue;
        }

        curchan.user_bytes_per_element = (fbslice->type == HALF) ? 2 : 4;
        curchan.user_data_type         = (exr_pixel_type_t)fbslice->type;
        curchan.user_pixel_stride      = fbslice->xStride;
        curchan.user_line_stride       = fbslice->yStride;

        ptr  = reinterpret_cast<uint8_t*> (fbslice->base);
        ptr += int64_t (cinfo.start_x / fbslice->xSampling) * int64_t (fbslice->xStride);
        ptr += int64_t (fbY / fbslice->ySampling) * int64_t (fbslice->yStride);

        curchan.decode_to_ptr = ptr;
    }
}

////////////////////////////////////////

void ScanLineProcess::run_fill (
    const FrameBuffer *outfb,
    int fbY,
    const std::vector<Slice> &filllist)
{
    for (auto& s: filllist)
    {
        uint8_t*       ptr;

        ptr  = reinterpret_cast<uint8_t*> (s.base);
        ptr += int64_t (cinfo.start_x / s.xSampling) * int64_t (s.xStride);
        ptr += int64_t (fbY / s.ySampling) * int64_t (s.yStride);

        // TODO: update ImfMisc, lift fill type / value
        int stop = cinfo.start_y + cinfo.height - decoder.user_line_end_ignore;
        for ( int start = fbY; start < stop; ++start )
        {
            if (start % s.ySampling) continue;

            uint8_t* outptr = ptr;
            for ( int sx = cinfo.start_x, ex = cinfo.start_x + cinfo.width;
                  sx < ex; ++sx )
            {
                if (sx % s.xSampling) continue;

                switch (s.type)
                {
                    case OPENEXR_IMF_INTERNAL_NAMESPACE::UINT:
                    {
                        unsigned int fillVal = (unsigned int) (s.fillValue);
                        *(unsigned int*)outptr = fillVal;
                        break;
                    }

                    case OPENEXR_IMF_INTERNAL_NAMESPACE::HALF:
                    {
                        half fillVal = half (s.fillValue);
                        *(half*)outptr = fillVal;
                        break;
                    }

                    case OPENEXR_IMF_INTERNAL_NAMESPACE::FLOAT:
                    {
                        float fillVal = float (s.fillValue);
                        *(float*)outptr = fillVal;
                        break;
                    }
                    default:
                        throw IEX_NAMESPACE::ArgExc ("Unknown pixel data type.");
                }
                outptr += s.xStride;
            }

            ptr += s.yStride;
        }
    }
}

OPENEXR_IMF_INTERNAL_NAMESPACE_SOURCE_EXIT
