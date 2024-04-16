//
// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) Contributors to the OpenEXR Project.
//

#include "ImfContext.h"

#include "Iex.h"

// TODO: remove these once we've cleared the legacy stream need
#include "ImfIO.h"
#include "ImfStdIO.h"
#include <mutex>

#include <string.h>
#include <string_view>

#include "openexr.h"

#include <ImfBoxAttribute.h>
#include <ImfChannelListAttribute.h>
#include <ImfChromaticitiesAttribute.h>
#include <ImfCompressionAttribute.h>
#include <ImfCompressor.h>
#include <ImfDeepImageStateAttribute.h>
#include <ImfDoubleAttribute.h>
#include <ImfDwaCompressor.h>
#include <ImfEnvmapAttribute.h>
#include <ImfFloatAttribute.h>
#include <ImfFloatVectorAttribute.h>
#include <ImfIDManifestAttribute.h>
#include <ImfIntAttribute.h>
#include <ImfKeyCodeAttribute.h>
#include <ImfLineOrderAttribute.h>
#include <ImfMatrixAttribute.h>
#include <ImfOpaqueAttribute.h>
#include <ImfPreviewImageAttribute.h>
#include <ImfRationalAttribute.h>
#include <ImfStringAttribute.h>
#include <ImfStringVectorAttribute.h>
#include <ImfTileDescriptionAttribute.h>
#include <ImfTimeCodeAttribute.h>
#include <ImfVecAttribute.h>

OPENEXR_IMF_INTERNAL_NAMESPACE_SOURCE_ENTER

namespace
{

/// TODO: delete this after porting of other classes has finished
class LegacyStream final : public IStream
{
public:
    LegacyStream (
        const char*                      fileName,
        exr_context_t                    ctxt,
        const exr_context_initializer_t& ctxtinit)
        : IStream (fileName)
        , _curpos (0)
        , _ctxt (ctxt)
        , _user_data (ctxtinit.user_data)
        , _read_fn (ctxtinit.read_fn)
    {
        if (!_read_fn) _stdstream = std::make_unique<StdIFStream> (fileName);
    }
    ~LegacyStream () override {}

    bool read (char c[/*n*/], int n) override
    {
        std::lock_guard<std::mutex> lk (_mx);
        if (_stdstream) return _stdstream->read (c, n);

        int64_t nr = _read_fn (_ctxt, _user_data, c, n, _curpos, nullptr);
        if (nr > 0) _curpos += nr;
        return nr == n;
    }

    uint64_t tellg () override
    {
        std::lock_guard<std::mutex> lk (_mx);
        if (_stdstream) return _stdstream->tellg ();
        return _curpos;
    }

    void seekg (uint64_t pos) override
    {
        std::lock_guard<std::mutex> lk (_mx);
        if (_stdstream) return _stdstream->seekg (pos);
        _curpos = pos;
    }

    void clear () override
    {
        std::lock_guard<std::mutex> lk (_mx);
        if (_stdstream) return _stdstream->clear ();
    }

private:
    std::mutex _mx;
    uint64_t   _curpos = 0;

    std::unique_ptr<IStream> _stdstream;

    exr_context_t       _ctxt      = nullptr;
    void*               _user_data = nullptr;
    exr_read_func_ptr_t _read_fn   = nullptr;
}; // class LegacyStream

} // namespace
////////////////////////////////////////

Context::Context ()
    : _ctxt (new exr_context_t, [] (exr_context_t* todel) {
        exr_finish (todel);
        delete todel;
    })
{
    *_ctxt = nullptr;
}

////////////////////////////////////////

void
Context::setLongNameSupport (bool onoff)
{
    if (EXR_ERR_SUCCESS != exr_set_longname_support (*_ctxt, onoff ? 1 : 0))
    {
        THROW (IEX_NAMESPACE::ArgExc, "Unable to set long name support flag");
    }
}

////////////////////////////////////////

void
Context::startRead (const char* filename, const ContextInitializer& ctxtinit)
{
    if (*_ctxt)
    {
        THROW (
            IEX_NAMESPACE::ArgExc, "Context already started, only start once");
    }

    if (EXR_ERR_SUCCESS !=
        exr_start_read (_ctxt.get (), filename, &(ctxtinit._initializer)))
    {
        THROW (
            IEX_NAMESPACE::InputExc,
            "Unable to open '" << filename << "' for read");
    }

    _prov_stream = ctxtinit._prov_stream;
    if (_prov_stream)
    {
        _prov_stream->seekg (0);
        _prov_stream->clear ();
    }
    else
    {
        _legacy = std::make_shared<LegacyStream> (
            filename, *_ctxt, ctxtinit._initializer);
    }
}

////////////////////////////////////////

void
Context::startWrite (const char* filename, const ContextInitializer& ctxtinit)
{
    if (*_ctxt)
    {
        THROW (
            IEX_NAMESPACE::ArgExc, "Context already started, only start once");
    }

    _legacy.reset ();

    if (EXR_ERR_SUCCESS != exr_start_write (
                               _ctxt.get (),
                               filename,
                               EXR_WRITE_FILE_DIRECTLY,
                               &(ctxtinit._initializer)))
    {
        THROW (
            IEX_NAMESPACE::InputExc,
            "Unable to open '" << filename << "' for write");
    }
}

////////////////////////////////////////

const char*
Context::fileName () const
{
    const char* filename = nullptr;

    if (EXR_ERR_SUCCESS != exr_get_file_name (*_ctxt, &filename))
    {
        THROW (IEX_NAMESPACE::ArgExc, "Unable to get filename from context");
    }

    return filename;
}

////////////////////////////////////////

int
Context::version () const
{
    uint32_t ver = 0;

    if (EXR_ERR_SUCCESS != exr_get_file_version_and_flags (*_ctxt, &ver))
    {
        THROW (
            IEX_NAMESPACE::ArgExc, "Unable to get file version from context");
    }

    return static_cast<int> (ver);
}

////////////////////////////////////////

int
Context::partCount () const
{
    int count = 0;

    if (EXR_ERR_SUCCESS != exr_get_count (*_ctxt, &count))
    {
        THROW (
            IEX_NAMESPACE::ArgExc,
            "Unable to get part count for file '" << fileName () << "'");
    }

    return count;
}

////////////////////////////////////////

exr_storage_t
Context::storage (int partidx) const
{
    exr_storage_t storage = EXR_STORAGE_LAST_TYPE;

    if (EXR_ERR_SUCCESS != exr_get_storage (*_ctxt, partidx, &storage))
    {
        THROW (
            IEX_NAMESPACE::ArgExc,
            "Unable to get storage type for part " << partidx << " in file '"
                                                   << fileName () << "'");
    }

    return storage;
}

////////////////////////////////////////

exr_attr_box2i_t
Context::dataWindow (int partidx) const
{
    exr_attr_box2i_t dw;

    if (EXR_ERR_SUCCESS != exr_get_data_window (*_ctxt, partidx, &dw))
    {
        THROW (
            IEX_NAMESPACE::ArgExc,
            "Unable to get the data window for part " << partidx << " in file '"
                                                      << fileName () << "'");
    }

    return dw;
}

////////////////////////////////////////

const exr_attr_chlist_t*
Context::channels (int partidx) const
{
    const exr_attr_chlist_t* cl;

    if (EXR_ERR_SUCCESS != exr_get_channels (*_ctxt, partidx, &cl))
    {
        THROW (
            IEX_NAMESPACE::ArgExc,
            "Unable to get the channel list for part "
                << partidx << " in file '" << fileName () << "'");
    }

    return cl;
}

////////////////////////////////////////

bool
Context::hasChannel (int partidx, const char* name) const
{
    const exr_attr_chlist_t* cl  = channels (partidx);
    int32_t                  len = strlen (name);

    for (int i = 0; i < cl->num_channels; ++i)
    {
        const exr_attr_chlist_entry_t* curc = cl->entries + i;
        if (curc->name.length == len && 0 == memcmp (name, curc->name.str, len))
        {
            return true;
        }
    }
    return false;
}

////////////////////////////////////////

exr_lineorder_t
Context::lineOrder (int partidx) const
{
    exr_lineorder_t lo;

    if (EXR_ERR_SUCCESS != exr_get_lineorder (*_ctxt, partidx, &lo))
    {
        THROW (
            IEX_NAMESPACE::ArgExc,
            "Unable to get the line order for part " << partidx << " in file '"
                                                     << fileName () << "'");
    }

    return lo;
}

////////////////////////////////////////

int
Context::attrCount (int partidx) const
{
    int32_t attrcnt = 0;

    if (EXR_ERR_SUCCESS != exr_get_attribute_count (*_ctxt, partidx, &attrcnt))
    {
        THROW (
            IEX_NAMESPACE::ArgExc,
            "Unable to get attribute for part " << partidx << " in file '"
                                                << fileName () << "'");
    }

    return 0;
}

////////////////////////////////////////

const exr_attribute_t*
Context::getAttr (int partidx, int attridx) const
{
    const exr_attribute_t* attr = nullptr;

    if (EXR_ERR_SUCCESS !=
        exr_get_attribute_by_index (
            *_ctxt, partidx, EXR_ATTR_LIST_FILE_ORDER, attridx, &attr))
    {
        THROW (
            IEX_NAMESPACE::ArgExc,
            "Unable to get attribute index " << attridx << " for part "
                                             << partidx << " in file '"
                                             << fileName () << "'");
    }

    return attr;
}

////////////////////////////////////////

const exr_attribute_t*
Context::getAttr (int partidx, const char* name) const
{
    const exr_attribute_t* attr = nullptr;
    exr_result_t           res;

    res = exr_get_attribute_by_name (*_ctxt, partidx, name, &attr);

    if (res == EXR_ERR_SUCCESS || res == EXR_ERR_NO_ATTR_BY_NAME) return attr;

    THROW (
        IEX_NAMESPACE::ArgExc,
        "Unable to find attribute '" << name << "' for part " << partidx
                                     << " in file '" << fileName () << "'");
}

////////////////////////////////////////

bool
Context::chunkTableValid (int partidx) const
{
    return exr_validate_chunk_table (*_ctxt, partidx) == EXR_ERR_SUCCESS;
}

////////////////////////////////////////

Header
Context::header (int partidx) const
{
    Header                 hdr;
    int32_t                attrcnt = 0;
    const exr_attribute_t* cur     = nullptr;

    if (EXR_ERR_SUCCESS != exr_get_attribute_count (*_ctxt, partidx, &attrcnt))
    {
        THROW (
            IEX_NAMESPACE::ArgExc,
            "Unable to get attribute for part " << partidx << " in file '"
                                                << fileName () << "'");
    }

    for (int32_t idx = 0; idx < attrcnt; ++idx)
    {
        if (EXR_ERR_SUCCESS !=
            exr_get_attribute_by_index (
                *_ctxt, partidx, EXR_ATTR_LIST_FILE_ORDER, idx, &cur))
        {
            THROW (
                IEX_NAMESPACE::ArgExc,
                "Unable to get attribute index " << idx << " for part "
                                                 << partidx << " in file '"
                                                 << fileName () << "'");
        }

        switch (cur->type)
        {
            case EXR_ATTR_INT:
                hdr.insert (cur->name, IntAttribute (cur->i));
                break;
            case EXR_ATTR_DOUBLE:
                hdr.insert (cur->name, DoubleAttribute (cur->d));
                break;
            case EXR_ATTR_FLOAT:
                hdr.insert (cur->name, FloatAttribute (cur->f));
                break;

            case EXR_ATTR_BOX2I:
                hdr.insert (
                    cur->name,
                    Box2iAttribute (IMATH_NAMESPACE::Box2i (
                        IMATH_NAMESPACE::V2i (cur->box2i->min),
                        IMATH_NAMESPACE::V2i (cur->box2i->max))));
                break;
            case EXR_ATTR_BOX2F:
                hdr.insert (
                    cur->name,
                    Box2fAttribute (IMATH_NAMESPACE::Box2f (
                        IMATH_NAMESPACE::V2f (cur->box2f->min),
                        IMATH_NAMESPACE::V2f (cur->box2f->max))));
                break;

            case EXR_ATTR_V2I:
                hdr.insert (
                    cur->name,
                    V2iAttribute (IMATH_NAMESPACE::V2i (*(cur->v2i))));
                break;
            case EXR_ATTR_V2F:
                hdr.insert (
                    cur->name,
                    V2fAttribute (IMATH_NAMESPACE::V2f (*(cur->v2f))));
                break;
            case EXR_ATTR_V2D:
                hdr.insert (
                    cur->name,
                    V2dAttribute (IMATH_NAMESPACE::V2d (*(cur->v2d))));
                break;
            case EXR_ATTR_V3I:
                hdr.insert (
                    cur->name,
                    V3iAttribute (IMATH_NAMESPACE::V3i (*(cur->v3i))));
                break;
            case EXR_ATTR_V3F:
                hdr.insert (
                    cur->name,
                    V3fAttribute (IMATH_NAMESPACE::V3f (*(cur->v3f))));
                break;
            case EXR_ATTR_V3D:
                hdr.insert (
                    cur->name,
                    V3dAttribute (IMATH_NAMESPACE::V3d (*(cur->v3d))));
                break;

            case EXR_ATTR_STRING:
                hdr.insert (
                    cur->name,
                    StringAttribute (
                        std::string (cur->string->str, cur->string->length)));
                break;

            case EXR_ATTR_COMPRESSION:
                hdr.insert (
                    cur->name, CompressionAttribute (Compression (cur->uc)));
                break;
            case EXR_ATTR_ENVMAP:
                hdr.insert (cur->name, EnvmapAttribute (Envmap (cur->uc)));
                break;
            case EXR_ATTR_LINEORDER:
                hdr.insert (
                    cur->name, LineOrderAttribute (LineOrder (cur->uc)));
                break;

            case EXR_ATTR_CHLIST: {
                ChannelList& chans = hdr.channels ();

                for (int c = 0; c < cur->chlist->num_channels; ++c)
                {
                    const exr_attr_chlist_entry_t& curc =
                        cur->chlist->entries[c];
                    chans.insert (
                        curc.name.str,
                        Channel (
                            PixelType (curc.pixel_type),
                            curc.x_sampling,
                            curc.y_sampling,
                            curc.p_linear != 0));
                }
                break;
            }

            default: {
                THROW (IEX_NAMESPACE::LogicExc, "Not yet implemented");
            }
#if 0
            case EXR_ATTR_CHROMATICITIES:
                retval += sizeof (*(cur->chromaticities));
                break;
            case EXR_ATTR_FLOAT_VECTOR:
                retval += sizeof (float) * (size_t) (cur->floatvector->length);
                break;
            case EXR_ATTR_KEYCODE: retval += sizeof (*(cur->keycode)); break;
            case EXR_ATTR_M33F: retval += sizeof (*(cur->m33f)); break;
            case EXR_ATTR_M33D: retval += sizeof (*(cur->m33d)); break;
            case EXR_ATTR_M44F: retval += sizeof (*(cur->m44f)); break;
            case EXR_ATTR_M44D: retval += sizeof (*(cur->m44d)); break;
            case EXR_ATTR_PREVIEW:
                retval += (size_t) cur->preview->width *
                          (size_t) cur->preview->height * (size_t) 4;
                break;
            case EXR_ATTR_RATIONAL: retval += sizeof (*(cur->rational)); break;
            case EXR_ATTR_STRING_VECTOR:
                for (int s = 0; s < cur->stringvector->n_strings; ++s)
                {
                    retval += (size_t) cur->stringvector->strings[s].length;
                    retval += sizeof (int32_t);
                }
                break;
            case EXR_ATTR_TILEDESC: retval += sizeof (*(cur->tiledesc)); break;
            case EXR_ATTR_TIMECODE: retval += sizeof (*(cur->timecode)); break;
            case EXR_ATTR_OPAQUE:
                if (cur->opaque->packed_data)
                    retval += (size_t) cur->opaque->size;
                else if (cur->opaque->unpacked_data)
                {
                    int32_t sz = 0;
                    rv =
                        exr_attr_opaquedata_pack (ctxt, cur->opaque, &sz, NULL);
                    if (rv != EXR_ERR_SUCCESS) return rv;

                    retval += (size_t) sz;
                }
                break;
            case EXR_ATTR_UNKNOWN:
            case EXR_ATTR_LAST_KNOWN_TYPE:
            default:
                return ctxt->print_error (
                    ctxt,
                    EXR_ERR_INVALID_ARGUMENT,
                    "Invalid / unhandled type '%s' for attribute '%s', unable to compute size",
                    cur->type_name,
                    cur->name);
#endif
        }
    }

    return hdr;
}

IStream*
Context::legacyIStream (int partnum) const
{
    IStream* ret;
    uint64_t coff = 0;

    if (EXR_ERR_SUCCESS != exr_get_chunk_table_offset (*_ctxt, partnum, &coff))
    {
        THROW (
            IEX_NAMESPACE::ArgExc,
            "Unable to get the chunk table offset for part "
                << partnum << " in file '" << fileName () << "'");
    }
    // The legacy system was built on a presumption of stateful
    // position within a stream, not arbitrary pread semantics
    // so we need to seek to the beginning of the chunk table
    // (aka end of header)
    ret = _prov_stream ? _prov_stream : _legacy.get ();
    ret->seekg (coff);
    return ret;
}

OPENEXR_IMF_INTERNAL_NAMESPACE_SOURCE_EXIT
