/*
 * Copyright © 2018 Adobe Systems Incorporated.
 *
 *  This is part of HarfBuzz, a text shaping library.
 *
 * Permission is hereby granted, without written agreement and without
 * license or royalty fees, to use, copy, modify, and distribute this
 * software and its documentation for any purpose, provided that the
 * above copyright notice and the following two paragraphs appear in
 * all copies of this software.
 *
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER BE LIABLE TO ANY PARTY FOR
 * DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES
 * ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION, EVEN
 * IF THE COPYRIGHT HOLDER HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * THE COPYRIGHT HOLDER SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING,
 * BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS
 * ON AN "AS IS" BASIS, AND THE COPYRIGHT HOLDER HAS NO OBLIGATION TO
 * PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
 *
 * Adobe Author(s): Michiharu Ariza
 */

#include "hb-open-type-private.hh"
#include "hb-ot-cff2-table.hh"
#include "hb-set.h"
#include "hb-subset-cff2.hh"
#include "hb-subset-plan.hh"
#include "hb-subset-cff-common-private.hh"

using namespace CFF;

struct CFF2SubTableOffsets {
  inline CFF2SubTableOffsets (void)
  {
    memset (this, 0, sizeof(*this));
  }

  unsigned int  topDictSize;
  unsigned int  varStoreOffset;
  TableInfo     FDSelectInfo;
  unsigned int  FDArrayOffset;
  unsigned int  FDArrayOffSize;
  unsigned int  charStringsOffset;
  unsigned int  charStringsOffSize;
  unsigned int  privateDictsOffset;
};

struct CFF2TopDict_OpSerializer : OpSerializer
{
  inline bool serialize (hb_serialize_context_t *c,
                         const OpStr &opstr,
                         const CFF2SubTableOffsets &offsets) const
  {
    TRACE_SERIALIZE (this);

    switch (opstr.op)
    {
      case OpCode_vstore:
        return_trace (FontDict::serialize_offset4_op(c, opstr.op, offsets.varStoreOffset));

      case OpCode_CharStrings:
        return_trace (FontDict::serialize_offset4_op(c, opstr.op, offsets.charStringsOffset));

      case OpCode_FDArray:
        return_trace (FontDict::serialize_offset4_op(c, opstr.op, offsets.FDArrayOffset));

      case OpCode_FDSelect:
        return_trace (FontDict::serialize_offset4_op(c, opstr.op, offsets.FDSelectInfo.offset));

      default:
        return_trace (copy_opstr (c, opstr));
    }
    return_trace (true);
  }

  inline unsigned int calculate_serialized_size (const OpStr &opstr) const
  {
    switch (opstr.op)
    {
      case OpCode_vstore:
      case OpCode_CharStrings:
      case OpCode_FDArray:
      case OpCode_FDSelect:
        return OpCode_Size (OpCode_longint) + 4 + OpCode_Size (opstr.op);
    
      default:
        return opstr.str.len;
    }
  }
};

struct CFF2FontDict_OpSerializer : OpSerializer
{
  inline bool serialize (hb_serialize_context_t *c,
                         const OpStr &opstr,
                         const TableInfo& privDictInfo) const
  {
    TRACE_SERIALIZE (this);

    if (opstr.op == OpCode_Private)
    {
      /* serialize the private dict size as a 2-byte integer */
      if (unlikely (!UnsizedByteStr::serialize_int2 (c, privDictInfo.size)))
        return_trace (false);

      /* serialize the private dict offset as a 4-byte integer */
      if (unlikely (!UnsizedByteStr::serialize_int4 (c, privDictInfo.offset)))
        return_trace (false);

      /* serialize the opcode */
      HBUINT8 *p = c->allocate_size<HBUINT8> (1);
      if (unlikely (p == nullptr)) return_trace (false);
      p->set (OpCode_Private);

      return_trace (true);
    }
    else
    {
      HBUINT8 *d = c->allocate_size<HBUINT8> (opstr.str.len);
      if (unlikely (d == nullptr)) return_trace (false);
      memcpy (d, &opstr.str.str[0], opstr.str.len);
    }
    return_trace (true);
  }

  inline unsigned int calculate_serialized_size (const OpStr &opstr) const
  {
    if (opstr.op == OpCode_Private)
      return OpCode_Size (OpCode_longint) + 4 + OpCode_Size (OpCode_shortint) + 2 + OpCode_Size (OpCode_Private);
    else
      return opstr.str.len;
  }
};

struct CFF2PrivateDict_OpSerializer : OpSerializer
{
  inline bool serialize (hb_serialize_context_t *c,
                         const OpStr &opstr,
                         const unsigned int subrsOffset) const
  {
    TRACE_SERIALIZE (this);

    if (opstr.op == OpCode_Subrs)
      return_trace (FontDict::serialize_offset2_op(c, OpCode_Subrs, subrsOffset));
    else
      return_trace (copy_opstr (c, opstr));
  }

  inline unsigned int calculate_serialized_size (const OpStr &opstr) const
  {
    if (opstr.op == OpCode_Subrs)
      return OpCode_Size (OpCode_shortint) + 2 + OpCode_Size (OpCode_Subrs);
    else
      return opstr.str.len;
  }
};

struct cff2_subset_plan {
  inline cff2_subset_plan (void)
    : final_size (0),
      orig_fdcount (0),
      subst_fdcount(1),
      subst_fdselect_format (0)
  {
    subst_fdselect_first_glyphs.init ();
    fdmap.init ();
    subset_charstrings.init ();
    privateDictInfos.init ();
  }

  inline ~cff2_subset_plan (void)
  {
    subst_fdselect_first_glyphs.fini ();
    fdmap.fini ();
    subset_charstrings.fini ();
    privateDictInfos.fini ();
  }

  inline bool create (const OT::cff2::accelerator_subset_t &acc,
              hb_subset_plan_t *plan)
  {
    final_size = 0;
    orig_fdcount = acc.fdArray->count;

    /* CFF2 header */
    final_size += OT::cff2::static_size;
    
    /* top dict */
    {
      CFF2TopDict_OpSerializer topSzr;
      offsets.topDictSize = TopDict::calculate_serialized_size (acc.topDict, topSzr);
      final_size += offsets.topDictSize;
    }

    /* global subrs */
    final_size += acc.globalSubrs->get_size ();

    /* variation store */
    if (acc.varStore != &Null(CFF2VariationStore))
    {
      offsets.varStoreOffset = final_size;
      final_size += acc.varStore->get_size ();
    }

    /* FDSelect */
    if (acc.fdSelect != &Null(CFF2FDSelect))
    {
      offsets.FDSelectInfo.offset = final_size;
      if (unlikely (!hb_plan_subset_cff_fdselect (plan->glyphs,
                                  orig_fdcount,
                                  *(const FDSelect *)acc.fdSelect,
                                  subst_fdcount,
                                  offsets.FDSelectInfo.size,
                                  subst_fdselect_format,
                                  subst_fdselect_first_glyphs,
                                  fdmap)))
        return false;
      
      if (!is_fds_subsetted ())
        offsets.FDSelectInfo.size = acc.fdSelect->calculate_serialized_size (acc.num_glyphs);
      final_size += offsets.FDSelectInfo.size;
    }

    /* FDArray (FDIndex) */
    {
      offsets.FDArrayOffset = final_size;
      CFF2FontDict_OpSerializer fontSzr;
      final_size += CFF2FDArray::calculate_serialized_size(offsets.FDArrayOffSize/*OUT*/, acc.fontDicts, subst_fdcount, fdmap, fontSzr);
    }

    /* CharStrings */
    {
      offsets.charStringsOffset = final_size;
      unsigned int dataSize = 0;
      for (unsigned int i = 0; i < plan->glyphs.len; i++)
      {
        const ByteStr str = (*acc.charStrings)[plan->glyphs[i]];
        subset_charstrings.push (str);
        dataSize += str.len;
      }
      offsets.charStringsOffSize = calcOffSize (dataSize + 1);
      final_size += CFF2CharStrings::calculate_serialized_size (offsets.charStringsOffSize, plan->glyphs.len, dataSize);
    }

    /* private dicts & local subrs */
    offsets.privateDictsOffset = final_size;
    for (unsigned int i = 0; i < orig_fdcount; i++)
    {
      CFF2PrivateDict_OpSerializer privSzr;
      TableInfo  privInfo = { final_size, PrivateDict::calculate_serialized_size (acc.privateDicts[i], privSzr) };
      privateDictInfos.push (privInfo);
      final_size += privInfo.size + acc.privateDicts[i].localSubrs->get_size ();
    }

    return true;
  }

  inline unsigned int get_final_size (void) const  { return final_size; }

  unsigned int        final_size;
  CFF2SubTableOffsets offsets;

  unsigned int    orig_fdcount;
  unsigned int    subst_fdcount;
  inline bool     is_fds_subsetted (void) const { return subst_fdcount < orig_fdcount; }
  unsigned int    subst_fdselect_format;
  hb_vector_t<hb_codepoint_t>   subst_fdselect_first_glyphs;

  /* font dict index remap table from fullset FDArray to subset FDArray.
   * set to HB_SET_VALUE_INVALID if excluded from subset */
  hb_vector_t<hb_codepoint_t>   fdmap;

  hb_vector_t<ByteStr> subset_charstrings;
  hb_vector_t<TableInfo> privateDictInfos;
};

static inline bool _write_cff2 (const cff2_subset_plan &plan,
                                const OT::cff2::accelerator_subset_t  &acc,
                                const hb_vector_t<hb_codepoint_t>& glyphs,
                                unsigned int dest_sz,
                                void *dest)
{
  hb_serialize_context_t c (dest, dest_sz);

  OT::cff2 *cff2 = c.start_serialize<OT::cff2> ();
  if (unlikely (!c.extend_min (*cff2)))
    return false;

  /* header */
  cff2->version.major.set (0x02);
  cff2->version.minor.set (0x00);
  cff2->topDict.set (OT::cff2::static_size);

  /* top dict */
  {
    assert (cff2->topDict == c.head - c.start);
    cff2->topDictSize.set (plan.offsets.topDictSize);
    TopDict &dict = cff2 + cff2->topDict;
    CFF2TopDict_OpSerializer topSzr;
    if (unlikely (!dict.serialize (&c, acc.topDict, topSzr, plan.offsets)))
    {
      DEBUG_MSG (SUBSET, nullptr, "failed to serialize CFF2 top dict");
      return false;
    }
  }

  /* global subrs */
  {
    assert (cff2->topDict + plan.offsets.topDictSize == c.head - c.start);
    CFF2Subrs *dest = c.start_embed<CFF2Subrs> ();
    if (unlikely (dest == nullptr)) return false;
    if (unlikely (!dest->serialize (&c, *acc.globalSubrs)))
    {
      DEBUG_MSG (SUBSET, nullptr, "failed to serialize CFF2 global subrs");
      return false;
    }
  }
  
  /* variation store */
  if (acc.varStore != &Null(CFF2VariationStore))
  {
    assert (plan.offsets.varStoreOffset == c.head - c.start);
    CFF2VariationStore *dest = c.start_embed<CFF2VariationStore> ();
    if (unlikely (!dest->serialize (&c, acc.varStore)))
    {
      DEBUG_MSG (SUBSET, nullptr, "failed to serialize CFF2 Variation Store");
      return false;
    }
  }

  /* FDSelect */
  if (acc.fdSelect != &Null(CFF2FDSelect))
  {
    assert (plan.offsets.FDSelectInfo.offset == c.head - c.start);
    
    if (plan.is_fds_subsetted ())
    {
      if (unlikely (!hb_serialize_cff_fdselect (&c, glyphs, *(const FDSelect *)acc.fdSelect, acc.fdArray->count,
                                                plan.subst_fdselect_format, plan.offsets.FDSelectInfo.size,
                                                plan.subst_fdselect_first_glyphs,
                                                plan.fdmap)))
      {
        DEBUG_MSG (SUBSET, nullptr, "failed to serialize CFF2 subset FDSelect");
        return false;
      }
    }
    else
    {
      CFF2FDSelect *dest = c.start_embed<CFF2FDSelect> ();
      if (unlikely (!dest->serialize (&c, *acc.fdSelect, acc.num_glyphs)))
      {
        DEBUG_MSG (SUBSET, nullptr, "failed to serialize CFF2 FDSelect");
        return false;
      }
    }
  }

  /* FDArray (FD Index) */
  {
    assert (plan.offsets.FDArrayOffset == c.head - c.start);
    CFF2FDArray  *fda = c.start_embed<CFF2FDArray> ();
    if (unlikely (fda == nullptr)) return false;
    CFF2FontDict_OpSerializer  fontSzr;
    if (unlikely (!fda->serialize (&c, plan.offsets.FDArrayOffSize,
                                   acc.fontDicts, plan.subst_fdcount, plan.fdmap,
                                   fontSzr, plan.privateDictInfos)))
    {
      DEBUG_MSG (SUBSET, nullptr, "failed to serialize CFF2 FDArray");
      return false;
    }
  }

  /* CharStrings */
  {
    assert (plan.offsets.charStringsOffset == c.head - c.start);
    CFF2CharStrings  *cs = c.start_embed<CFF2CharStrings> ();
    if (unlikely (cs == nullptr)) return false;
    if (unlikely (!cs->serialize (&c, plan.offsets.charStringsOffSize, plan.subset_charstrings)))
    {
      DEBUG_MSG (SUBSET, nullptr, "failed to serialize CFF2 CharStrings");
      return false;
    }
  }

  /* private dicts & local subrs */
  assert (plan.offsets.privateDictsOffset == c.head - c.start);
  for (unsigned int i = 0; i < acc.privateDicts.len; i++)
  {
    PrivateDict  *pd = c.start_embed<PrivateDict> ();
    if (unlikely (pd == nullptr)) return false;
    CFF2PrivateDict_OpSerializer privSzr;
    /* N.B. local subrs immediately follows its corresponding private dict. i.e., subr offset == private dict size */
    if (unlikely (!pd->serialize (&c, acc.privateDicts[i], privSzr, plan.privateDictInfos[i].size)))
    {
      DEBUG_MSG (SUBSET, nullptr, "failed to serialize CFF2 Private Dict[%d]", i);
      return false;
    }
    if (acc.privateDicts[i].subrsOffset != 0)
    {
      CFF2Subrs *subrs = c.start_embed<CFF2Subrs> ();
      if (unlikely (subrs == nullptr) || acc.privateDicts[i].localSubrs == &Null(CFF2Subrs))
      {
        DEBUG_MSG (SUBSET, nullptr, "CFF2 subset: local subrs unexpectedly null [%d]", i);
        return false;
      }
      if (unlikely (!subrs->serialize (&c, *acc.privateDicts[i].localSubrs)))
      {
        DEBUG_MSG (SUBSET, nullptr, "failed to serialize CFF2 local subrs [%d]", i);
        return false;
      }
    }
  }

  c.end_serialize ();

  return true;
}

static bool
_hb_subset_cff2 (const OT::cff2::accelerator_subset_t  &acc,
                const char                      *data,
                hb_subset_plan_t                *plan,
                hb_blob_t                       **prime /* OUT */)
{
  cff2_subset_plan cff2_plan;

  if (unlikely (!cff2_plan.create (acc, plan)))
  {
    DEBUG_MSG(SUBSET, nullptr, "Failed to generate a cff2 subsetting plan.");
    return false;
  }

  unsigned int  cff2_prime_size = cff2_plan.get_final_size ();
  char *cff2_prime_data = (char *) calloc (1, cff2_prime_size);

  if (unlikely (!_write_cff2 (cff2_plan, acc, plan->glyphs,
                              cff2_prime_size, cff2_prime_data))) {
    DEBUG_MSG(SUBSET, nullptr, "Failed to write a subset cff2.");
    free (cff2_prime_data);
    return false;
  }

  *prime = hb_blob_create (cff2_prime_data,
                                cff2_prime_size,
                                HB_MEMORY_MODE_READONLY,
                                cff2_prime_data,
                                free);
  return true;
}

/**
 * hb_subset_cff2:
 * Subsets the CFF2 table according to a provided plan.
 *
 * Return value: subsetted cff2 table.
 **/
bool
hb_subset_cff2 (hb_subset_plan_t *plan,
                hb_blob_t       **prime /* OUT */)
{
  hb_blob_t *cff2_blob = hb_sanitize_context_t().reference_table<CFF::cff2> (plan->source);
  const char *data = hb_blob_get_data(cff2_blob, nullptr);

  OT::cff2::accelerator_subset_t acc;
  acc.init(plan->source);
  bool result = likely (acc.is_valid ()) &&
                _hb_subset_cff2 (acc, data, plan, prime);

  hb_blob_destroy (cff2_blob);
  acc.fini ();

  return result;
}
