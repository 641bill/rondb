/* Copyright (c) 2004, 2016, Oracle and/or its affiliates. All rights reserved.
   
   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; version 2
   of the License.
   
   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.
   
   You should have received a copy of the GNU Library General Public
   License along with this library; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/* 
   UCA (Unicode Collation Algorithm) support. 
   Written by Alexander Barkov <bar@mysql.com>
   
   Currently supports only subset of the full UCA:
   - Only Primary level key comparison
   - Basic Latin letters contraction is implemented
   - Variable weighting is done for Non-ignorable option
   
   Features that are not implemented yet:
   - No Normalization From D is done
     + No decomposition is done
     + No Thai/Lao orderding is done
   - No combining marks processing is done
*/


#include <my_global.h>
#include <string.h>
#include <sys/types.h>
#include <algorithm>
#include <bitset>
#include <iterator>

#include "m_ctype.h"
#include "m_string.h"
#include "mb_wc.h"
#include "my_byteorder.h"
#include "my_dbug.h"
#include "my_loglevel.h"
#include "mysql/service_my_snprintf.h"
#include "str_uca_type.h"
#include "template_utils.h"
#include "uca900_data.h"
#include "uca_data.h"

MY_UCA_INFO my_uca_v400=
{
  UCA_V400,
  {
    {
      0xFFFF,    /* maxchar           */
      uca_length,
      uca_weight,
      {          /* Contractions:     */
        0,       /*   nitems          */
        NULL,    /*   item            */
        NULL     /*   flags           */
      }
    },
  },

  /* Logical positions */
  0x0009,    /* first_non_ignorable       p != ignore                  */
  0xA48C,    /* last_non_ignorable        Not a CJK and not UNASSIGNED */

  0x0332,    /* first_primary_ignorable   p == 0                       */
  0x20EA,    /* last_primary_ignorable                                 */

  0x0000,    /* first_secondary_ignorable p,s == 0                     */
  0xFE73,    /* last_secondary_ignorable  p,s == 0                     */

  0x0000,    /* first_tertiary_ignorable  p,s,t == 0                   */
  0xFE73,    /* last_tertiary_ignorable   p,s,t == 0                   */

  0x0000,    /* first_trailing            */
  0x0000,    /* last_trailing             */

  0x0009,    /* first_variable            */
  0x2183,    /* last_variable             */
};

/******************************************************/


MY_UCA_INFO my_uca_v520=
{
  UCA_V520,
  {
    {
      0x10FFFF,      /* maxchar           */
      uca520_length,
      uca520_weight,
      {              /* Contractions:     */
        0,           /*   nitems          */
        NULL,        /*   item            */
        NULL         /*   flags           */
      }
    },
  },

  0x0009,    /* first_non_ignorable       p != ignore                       */
  0x1342E,   /* last_non_ignorable        Not a CJK and not UASSIGNED       */

  0x0332,    /* first_primary_ignorable   p == ignore                       */
  0x101FD,   /* last_primary_ignorable                                      */

  0x0000,    /* first_secondary_ignorable p,s= ignore                       */
  0xFE73,    /* last_secondary_ignorable                                    */

  0x0000,    /* first_tertiary_ignorable  p,s,t == ignore                   */
  0xFE73,    /* last_tertiary_ignorable                                     */

  0x0000,    /* first_trailing                                              */
  0x0000,    /* last_trailing                                               */

  0x0009,    /* first_variable            if alt=non-ignorable: p != ignore */
  0x1D371,   /* last_variable             if alt=shifter: p,s,t == ignore   */
};


/******************************************************/

/*
  German Phonebook
*/
static const char german2[]=
    "&AE << \\u00E6 <<< \\u00C6 << \\u00E4 <<< \\u00C4 "
    "&OE << \\u0153 <<< \\u0152 << \\u00F6 <<< \\u00D6 "
    "&UE << \\u00FC <<< \\u00DC ";


/*
  Some sources treat LETTER A WITH DIARESIS (00E4,00C4)
  secondary greater than LETTER AE (00E6,00C6).
  http://www.evertype.com/alphabets/icelandic.pdf
  http://developer.mimer.com/collations/charts/icelandic.htm

  Other sources do not provide any special rules
  for LETTER A WITH DIARESIS:
  http://www.omniglot.com/writing/icelandic.htm
  http://en.wikipedia.org/wiki/Icelandic_alphabet
  http://oss.software.ibm.com/icu/charts/collation/is.html

  Let's go the first way.
*/

static const char icelandic[]=
    "& A < \\u00E1 <<< \\u00C1 "
    "& D < \\u00F0 <<< \\u00D0 "
    "& E < \\u00E9 <<< \\u00C9 "
    "& I < \\u00ED <<< \\u00CD "
    "& O < \\u00F3 <<< \\u00D3 "
    "& U < \\u00FA <<< \\u00DA "
    "& Y < \\u00FD <<< \\u00DD "
    "& Z < \\u00FE <<< \\u00DE "
        "< \\u00E6 <<< \\u00C6 << \\u00E4 <<< \\u00C4 "
        "< \\u00F6 <<< \\u00D6 << \\u00F8 <<< \\u00D8 "
        "< \\u00E5 <<< \\u00C5 ";

/*
  Some sources treat I and Y primary different.
  Other sources treat I and Y the same on primary level.
  We'll go the first way.
*/

static const char latvian[]=
    "& C < \\u010D <<< \\u010C "
    "& G < \\u0123 <<< \\u0122 "
    "& I < \\u0079 <<< \\u0059 "
    "& K < \\u0137 <<< \\u0136 "
    "& L < \\u013C <<< \\u013B "
    "& N < \\u0146 <<< \\u0145 "
    "& R < \\u0157 <<< \\u0156 "
    "& S < \\u0161 <<< \\u0160 "
    "& Z < \\u017E <<< \\u017D ";


static const char romanian[]=
    "& A < \\u0103 <<< \\u0102 < \\u00E2 <<< \\u00C2 "
    "& I < \\u00EE <<< \\u00CE "
    "& S < \\u0219 <<< \\u0218 << \\u015F <<< \\u015E "
    "& T < \\u021B <<< \\u021A << \\u0163 <<< \\u0162 ";

static const char slovenian[]=
    "& C < \\u010D <<< \\u010C "
    "& S < \\u0161 <<< \\u0160 "
    "& Z < \\u017E <<< \\u017D ";
    

static const char polish[]=
    "& A < \\u0105 <<< \\u0104 "
    "& C < \\u0107 <<< \\u0106 "
    "& E < \\u0119 <<< \\u0118 "
    "& L < \\u0142 <<< \\u0141 "
    "& N < \\u0144 <<< \\u0143 "
    "& O < \\u00F3 <<< \\u00D3 "
    "& S < \\u015B <<< \\u015A "
    "& Z < \\u017A <<< \\u0179 < \\u017C <<< \\u017B";

static const char estonian[]=
    "& S < \\u0161 <<< \\u0160 "
       " < \\u007A <<< \\u005A "
       " < \\u017E <<< \\u017D "
    "& W < \\u00F5 <<< \\u00D5 "
        "< \\u00E4 <<< \\u00C4 "
        "< \\u00F6 <<< \\u00D6 "
        "< \\u00FC <<< \\u00DC ";

static const char spanish[]= "& N < \\u00F1 <<< \\u00D1 ";

/*
  Some sources treat V and W as similar on primary level.
  We'll treat V and W as different on primary level.
*/
    
static const char swedish[]=
    "& Y <<\\u00FC <<< \\u00DC "
    "& Z < \\u00E5 <<< \\u00C5 "
        "< \\u00E4 <<< \\u00C4 << \\u00E6 <<< \\u00C6 "
        "< \\u00F6 <<< \\u00D6 << \\u00F8 <<< \\u00D8 ";

static const char turkish[]=
    "& C < \\u00E7 <<< \\u00C7 "
    "& G < \\u011F <<< \\u011E "
    "& H < \\u0131 <<< \\u0049 "
    "& O < \\u00F6 <<< \\u00D6 "
    "& S < \\u015F <<< \\u015E "
    "& U < \\u00FC <<< \\u00DC ";
    

static const char czech[]=
    "& C < \\u010D <<< \\u010C "
    "& H <      ch <<<      Ch <<< CH"
    "& R < \\u0159 <<< \\u0158"
    "& S < \\u0161 <<< \\u0160"
    "& Z < \\u017E <<< \\u017D";

static const char danish[]=  /* Also good for Norwegian */
    "& Y << \\u00FC <<< \\u00DC << \\u0171 <<< \\u0170"
    "& Z  < \\u00E6 <<< \\u00C6 << \\u00E4 <<< \\u00C4"
        " < \\u00F8 <<< \\u00D8 << \\u00F6 <<< \\u00D6 << \\u0151 <<< \\u0150"
        " < \\u00E5 <<< \\u00C5 << aa <<<  Aa <<< AA";

static const char lithuanian[]=
    "& C << ch <<< Ch <<< CH< \\u010D <<< \\u010C"
    "& E << \\u0119 <<< \\u0118 << \\u0117 <<< \\u0116"
    "& I << y <<< Y"
    "& S  < \\u0161 <<< \\u0160"
    "& Z  < \\u017E <<< \\u017D";

static const char slovak[]=
    "& A < \\u00E4 <<< \\u00C4"
    "& C < \\u010D <<< \\u010C"
    "& H < ch <<< Ch <<< CH"
    "& O < \\u00F4 <<< \\u00D4"
    "& S < \\u0161 <<< \\u0160"
    "& Z < \\u017E <<< \\u017D";

static const char spanish2[]=	/* Also good for Asturian and Galician */
    "&C <  ch <<< Ch <<< CH"
    "&L <  ll <<< Ll <<< LL"
    "&N < \\u00F1 <<< \\u00D1";

static const char roman[]= /* i.e. Classical Latin */
    "& I << j <<< J "
    "& V << u <<< U ";

/*
  Persian collation support was provided by 
  Jody McIntyre <mysql@modernduck.com>
  
  To: internals@lists.mysql.com
  Subject: Persian UTF8 collation support
  Date: 17.08.2004
  
  Contraction is not implemented.  Some implementations do perform
  contraction but others do not, and it is able to sort all my test
  strings correctly.
  
  Jody.
*/
static const char persian[]=
    "& \\u066D < \\u064E < \\uFE76 < \\uFE77 < \\u0650 < \\uFE7A < \\uFE7B"
             " < \\u064F < \\uFE78 < \\uFE79 < \\u064B < \\uFE70 < \\uFE71"
             " < \\u064D < \\uFE74 < \\u064C < \\uFE72"
    "& \\uFE7F < \\u0653 < \\u0654 < \\u0655 < \\u0670"
    "& \\u0669 < \\u0622 < \\u0627 < \\u0671 < \\u0621 < \\u0623 < \\u0625"
             " < \\u0624 < \\u0626"
    "& \\u0642 < \\u06A9 < \\u0643"
    "& \\u0648 < \\u0647 < \\u0629 < \\u06C0 < \\u06CC < \\u0649 < \\u064A"
    "& \\uFE80 < \\uFE81 < \\uFE82 < \\uFE8D < \\uFE8E < \\uFB50 < \\uFB51"
             " < \\uFE80 "
    /*
      FE80 appears both in reset and shift.
      We need to break the rule here and reset to *new* FE80 again,
      so weight for FE83 is calculated as P[FE80]+1, not as P[FE80]+8.
    */
             " & \\uFE80 < \\uFE83 < \\uFE84 < \\uFE87 < \\uFE88 < \\uFE85"
             " < \\uFE86 < \\u0689 < \\u068A"
    "& \\uFEAE < \\uFDFC"
    "& \\uFED8 < \\uFB8E < \\uFB8F < \\uFB90 < \\uFB91 < \\uFED9 < \\uFEDA"
             " < \\uFEDB < \\uFEDC"
    "& \\uFEEE < \\uFEE9 < \\uFEEA < \\uFEEB < \\uFEEC < \\uFE93 < \\uFE94"
             " < \\uFBA4 < \\uFBA5 < \\uFBFC < \\uFBFD < \\uFBFE < \\uFBFF"
             " < \\uFEEF < \\uFEF0 < \\uFEF1 < \\uFEF2 < \\uFEF3 < \\uFEF4"
             " < \\uFEF5 < \\uFEF6 < \\uFEF7 < \\uFEF8 < \\uFEF9 < \\uFEFA"
             " < \\uFEFB < \\uFEFC";

/*
  Esperanto tailoring.
  Contributed by Bertilo Wennergren <bertilow at gmail dot com>
  September 1, 2005
*/
static const char esperanto[]=
    "& C < \\u0109 <<< \\u0108"
    "& G < \\u011D <<< \\u011C"
    "& H < \\u0125 <<< \\u0124"
    "& J < \\u0135 <<< \\u0134"
    "& S < \\u015d <<< \\u015c"
    "& U < \\u016d <<< \\u016c";

/*
  A simplified version of Hungarian, without consonant contractions.
*/
static const char hungarian[]=
    "&O < \\u00F6 <<< \\u00D6 << \\u0151 <<< \\u0150"
    "&U < \\u00FC <<< \\u00DC << \\u0171 <<< \\u0170";


static const char croatian[]=
    "&C < \\u010D <<< \\u010C < \\u0107 <<< \\u0106"
    "&D < d\\u017E = \\u01C6 <<< d\\u017D <<< D\\u017E = \\u01C5 <<< D\\u017D = \\u01C4"
    "   < \\u0111 <<< \\u0110"
    "&L < lj = \\u01C9  <<< lJ <<< Lj = \\u01C8 <<< LJ = \\u01C7"
    "&N < nj = \\u01CC  <<< nJ <<< Nj = \\u01CB <<< NJ = \\u01CA"
    "&S < \\u0161 <<< \\u0160"
    "&Z < \\u017E <<< \\u017D";


/*
  SCCII Part 1 : Collation Sequence (SLS1134)
  2006/11/24
  Harshula Jayasuriya <harshula at gmail dot com>
  Language Technology Research Lab, University of Colombo / ICTA
*/
#if 0
static const char sinhala[]=
    "& \\u0D96 < \\u0D82 < \\u0D83"
    "& \\u0DA5 < \\u0DA4"
    "& \\u0DD8 < \\u0DF2 < \\u0DDF < \\u0DF3"
    "& \\u0DDE < \\u0DCA";
#else
static const char sinhala[]=
    "& \\u0D96 < \\u0D82 < \\u0D83 < \\u0D9A < \\u0D9B < \\u0D9C < \\u0D9D"
              "< \\u0D9E < \\u0D9F < \\u0DA0 < \\u0DA1 < \\u0DA2 < \\u0DA3"
              "< \\u0DA5 < \\u0DA4 < \\u0DA6"
              "< \\u0DA7 < \\u0DA8 < \\u0DA9 < \\u0DAA < \\u0DAB < \\u0DAC"
              "< \\u0DAD < \\u0DAE < \\u0DAF < \\u0DB0 < \\u0DB1"
              "< \\u0DB3 < \\u0DB4 < \\u0DB5 < \\u0DB6 < \\u0DB7 < \\u0DB8"
              "< \\u0DB9 < \\u0DBA < \\u0DBB < \\u0DBD < \\u0DC0 < \\u0DC1"
              "< \\u0DC2 < \\u0DC3 < \\u0DC4 < \\u0DC5 < \\u0DC6"
              "< \\u0DCF"
              "< \\u0DD0 < \\u0DD1 < \\u0DD2 < \\u0DD3 < \\u0DD4 < \\u0DD6"
              "< \\u0DD8 < \\u0DF2 < \\u0DDF < \\u0DF3 < \\u0DD9 < \\u0DDA"
              "< \\u0DDB < \\u0DDC < \\u0DDD < \\u0DDE < \\u0DCA";
#endif


static const char vietnamese[]=
" &A << \\u00E0 <<< \\u00C0" /* A */
   " << \\u1EA3 <<< \\u1EA2"
   " << \\u00E3 <<< \\u00C3"
   " << \\u00E1 <<< \\u00C1"
   " << \\u1EA1 <<< \\u1EA0"
   "  < \\u0103 <<< \\u0102" /* A WITH BREVE */
   " << \\u1EB1 <<< \\u1EB0"
   " << \\u1EB3 <<< \\u1EB2"
   " << \\u1EB5 <<< \\u1EB4"
   " << \\u1EAF <<< \\u1EAE"
   " << \\u1EB7 <<< \\u1EB6"
   "  < \\u00E2 <<< \\u00C2" /* A WITH CIRCUMFLEX */
   " << \\u1EA7 <<< \\u1EA6"
   " << \\u1EA9 <<< \\u1EA8"
   " << \\u1EAB <<< \\u1EAA"
   " << \\u1EA5 <<< \\u1EA4"
   " << \\u1EAD <<< \\u1EAC"
" &D  < \\u0111 <<< \\u0110" /* D WITH STROKE */
" &E << \\u00E8 <<< \\u00C8" /* E */
   " << \\u1EBB <<< \\u1EBA"
   " << \\u1EBD <<< \\u1EBC"
   " << \\u00E9 <<< \\u00C9"
   " << \\u1EB9 <<< \\u1EB8"
   "  < \\u00EA <<< \\u00CA" /* E WITH CIRCUMFLEX */
   " << \\u1EC1 <<< \\u1EC0"
   " << \\u1EC3 <<< \\u1EC2"
   " << \\u1EC5 <<< \\u1EC4"
   " << \\u1EBF <<< \\u1EBE"
   " << \\u1EC7 <<< \\u1EC6"
" &I << \\u00EC <<< \\u00CC" /* I */
   " << \\u1EC9 <<< \\u1EC8"
   " << \\u0129 <<< \\u0128"
   " << \\u00ED <<< \\u00CD"
   " << \\u1ECB <<< \\u1ECA"
" &O << \\u00F2 <<< \\u00D2" /* O */
   " << \\u1ECF <<< \\u1ECE"
   " << \\u00F5 <<< \\u00D5"
   " << \\u00F3 <<< \\u00D3"
   " << \\u1ECD <<< \\u1ECC"
   "  < \\u00F4 <<< \\u00D4" /* O WITH CIRCUMFLEX */
   " << \\u1ED3 <<< \\u1ED2"
   " << \\u1ED5 <<< \\u1ED4"
   " << \\u1ED7 <<< \\u1ED6"
   " << \\u1ED1 <<< \\u1ED0"
   " << \\u1ED9 <<< \\u1ED8"
   "  < \\u01A1 <<< \\u01A0" /* O WITH HORN */
   " << \\u1EDD <<< \\u1EDC"
   " << \\u1EDF <<< \\u1EDE"
   " << \\u1EE1 <<< \\u1EE0"
   " << \\u1EDB <<< \\u1EDA"
   " << \\u1EE3 <<< \\u1EE2"
" &U << \\u00F9 <<< \\u00D9" /* U */
   " << \\u1EE7 <<< \\u1EE6"
   " << \\u0169 <<< \\u0168"
   " << \\u00FA <<< \\u00DA"
   " << \\u1EE5 <<< \\u1EE4"
   "  < \\u01B0 <<< \\u01AF" /* U WITH HORN */
   " << \\u1EEB <<< \\u1EEA"
   " << \\u1EED <<< \\u1EEC"
   " << \\u1EEF <<< \\u1EEE"
   " << \\u1EE9 <<< \\u1EE8"
   " << \\u1EF1 <<< \\u1EF0"
" &Y << \\u1EF3 <<< \\u1EF2" /* Y */
   " << \\u1EF7 <<< \\u1EF6"
   " << \\u1EF9 <<< \\u1EF8"
   " << \\u00FD <<< \\u00DD"
   " << \\u1EF5 <<< \\u1EF4";

/* German Phonebook */
static const char de_pb_cldr_29[]=
  "&AE << \\u00E4 <<< \\u00C4 "
  "&OE << \\u00F6 <<< \\u00D6 "
  "&UE << \\u00FC <<< \\u00DC ";

/* Icelandic */
static const char is_cldr_29[]=
  "&[before 1]b       <  \\u00E1 <<< \\u00C1 "
  "&          d       << \\u0111 <<< \\u0110 < \\u00F0 <<< \\u00D0 "
  "&[before 1]f       <  \\u00E9 <<< \\u00C9 "
  "&[before 1]j       <  \\u00ED <<< \\u00CD "
  "&[before 1]p       <  \\u00F3 <<< \\u00D3 "
  "&[before 1]v       <  \\u00FA <<< \\u00DA "
  "&[before 1]z       <  \\u00FD <<< \\u00DD "
  "&[before 1]\\u01C0 <  \\u00E6 <<< \\u00C6 << \\u00E4 <<< \\u00C4 "
                     "<  \\u00F6 <<< \\u00D6 << \\u00F8 <<< \\u00D8 "
                     "<  \\u00E5 <<< \\u00C5";

/* Latvian */
static const char lv_cldr_29[]=
  "&[before 1]D       <  \\u010D <<< \\u010C "
  "&[before 1]H       <  \\u0123 <<< \\u0122 "
  "&          I       << y       <<< Y "
  "&[before 1]L       <  \\u0137 <<< \\u0136 "
  "&[before 1]M       <  \\u013C <<< \\u013B "
  "&[before 1]O       <  \\u0146 <<< \\u0145 "
  "&[before 1]S       <  \\u0157 <<< \\u0156 "
  "&[before 1]T       <  \\u0161 <<< \\u0160 "
  "&[before 1]\\u01B7 <  \\u017E <<< \\u017D";

/* Romanian */
static const char ro_cldr_29[]=
  "&A < \\u0103 <<< \\u0102 <   \\u00E2 <<< \\u00C2 "
  "&I < \\u00EE <<< \\u00CE "
  "&S < \\u015F =   \\u0219 <<< \\u015E =   \\u0218 "
  "&T < \\u0163 =   \\u021B <<< \\u0162 =   \\u021A";

/* Slovenian */
static const char sl_cldr_29[]=
  "&C < \\u010D <<< \\u010C < \\u0107 <<< \\u0106 "
  "&D < \\u0111 <<< \\u0110 "
  "&S < \\u0161 <<< \\u0160 "
  "&Z < \\u017E <<< \\u017D";

/* Polish */
static const char pl_cldr_29[]=
  "&A < \\u0105 <<< \\u0104 "
  "&C < \\u0107 <<< \\u0106 "
  "&E < \\u0119 <<< \\u0118 "
  "&L < \\u0142 <<< \\u0141 "
  "&N < \\u0144 <<< \\u0143 "
  "&O < \\u00F3 <<< \\u00D3 "
  "&S < \\u015B <<< \\u015A "
  "&Z < \\u017A <<< \\u0179 < \\u017C <<< \\u017B";

/* Estonian */
static const char et_cldr_29[]=
  "&[before 1]T <   \\u0161 <<< \\u0160 < z         <<< Z "
               "<   \\u017E <<< \\u017D "
  "&[before 1]X <   \\u00F5 <<< \\u00D5 <   \\u00E4 <<< \\u00C4 "
               "<   \\u00F6 <<< \\u00D6 <   \\u00FC <<< \\u00DC";

/* Swedish */
static const char sv_cldr_29[]=
  "&          D       <<  \\u0111   <<< \\u0110 <<  \\u00F0 <<< \\u00D0 "
  "&          t       <<< \\u00FE/h "
  "&          T       <<< \\u00DE/H "
  "&          Y       <<  \\u00FC   <<< \\u00DC <<  \\u0171 <<< \\u0170 "
  "&[before 1]\\u01C0 <   \\u00E5   <<< \\u00C5 <   \\u00E4 <<< \\u00C4 "
                      "<< \\u00E6   <<< \\u00C6 <<  \\u0119 <<< \\u0118 "
                      "<  \\u00F6   <<< \\u00D6 <<  \\u00F8 <<< \\u00D8 "
                      "<< \\u0151   <<< \\u0150 <<  \\u0153 <<< \\u0152 "
                      "<< \\u00F4   <<< \\u00D4";

/* Turkish */
static const char tr_cldr_29[]=
  "&          C <   \\u00E7 <<< \\u00C7 "
  "&          G <   \\u011F <<< \\u011E "
  "&[before 1]i <   \\u0131 <<< I "
  "&          i <<< \\u0130 "
  "&          O <   \\u00F6 <<< \\u00D6 "
  "&          S <   \\u015F <<< \\u015E "
  "&          U <   \\u00FC <<< \\u00DC ";

/* Czech */
static const char cs_cldr_29[]=
  "&C < \\u010D <<< \\u010C "
  "&H < ch      <<< cH       <<< Ch <<< CH "
  "&R < \\u0159 <<< \\u0158"
  "&S < \\u0161 <<< \\u0160"
  "&Z < \\u017E <<< \\u017D";

/* Danish */
static const char da_cldr_29[]=
  "&          D       <<  \\u0111   <<< \\u0110 <<  \\u00F0 <<< \\u00D0 "
  "&          t       <<< \\u00FE/h "
  "&          T       <<< \\u00DE/H "
  "&          Y       <<  \\u00FC   <<< \\u00DC <<  \\u0171 <<< \\u0170 "
  "&[before 1]\\u01C0 <   \\u00E6   <<< \\u00C6 <<  \\u00E4 <<< \\u00C4 "
                     "<   \\u00F8   <<< \\u00D8 <<  \\u00F6 <<< \\u00D6 "
                     "<<  \\u0151   <<< \\u0150 <<  \\u0153 <<< \\u0152 "
                     "<   \\u00E5   <<< \\u00C5 <<< aa      <<< Aa "
                     "<<< AA";

static Coll_param da_coll_param= {
  nullptr, FALSE, CASE_FIRST_UPPER
};

/* Lithuanian */
static const char lt_cldr_29[]=
  "&\\u0300 = \\u0307\\u0300 "
  "&\\u0301 = \\u0307\\u0301 "
  "&\\u0303 = \\u0307\\u0303 "
  "&A << \\u0105 <<< \\u0104 "
  "&C <  \\u010D <<< \\u010C "
  "&E << \\u0119 <<< \\u0118 << \\u0117 <<< \\u0116"
  "&I << \\u012F <<< \\u012E << y       <<< Y "
  "&S <  \\u0161 <<< \\u0160 "
  "&U << \\u0173 <<< \\u0172 << \\u016B <<< \\u016A "
  "&Z <  \\u017E <<< \\u017D";

/* Slovak */
static const char sk_cldr_29[]=
  "&A < \\u00E4 <<< \\u00C4 "
  "&C < \\u010D <<< \\u010C "
  "&H < ch      <<< cH      <<< Ch <<< CH "
  "&O < \\u00F4 <<< \\u00D4 "
  "&R < \\u0159 <<< \\u0158 "
  "&S < \\u0161 <<< \\u0160 "
  "&Z < \\u017E <<< \\u017D";

/* Spanish (Traditional) */
static const char es_trad_cldr_29[]=
  "&N <  \\u00F1 <<< \\u00D1 "
  "&C <  ch      <<< Ch      <<< CH "
  "&l <  ll      <<< Ll      <<< LL";

/* Persian */
#if 0
static const char fa_cldr_29[]=
  "&          \\u064E << \\u0650 << \\u064F <<  \\u064B << \\u064D "
                     "<< \\u064C "
  "&[before 1]\\u0627 <  \\u0622 "
  "&          \\u0627 << \\u0671 <  \\u0621 <<  \\u0623 << \\u0672 "
                     "<< \\u0625 << \\u0673 <<  \\u0624 << \\u06CC\\u0654 "
                     "<<< \\u0649\\u0654    <<< \\u0626 "
  "&          \\u06A9 << \\u06AA << \\u06AB <<  \\u0643 << \\u06AC "
                     "<< \\u06AD << \\u06AE "
  "&          \\u06CF <  \\u0647 << \\u06D5 <<  \\u06C1 << \\u0629 "
                     "<< \\u06C3 << \\u06C0 <<  \\u06BE "
  "&          \\u06CC << \\u0649 << \\u06D2 <<  \\u064A << \\u06D0 "
                     "<< \\u06D1 << \\u06CD <<  \\u06CE";

static Reorder_param fa_reorder_param= {
  {CHARGRP_ARAB, CHARGRP_NONE}, {{{0, 0}, {0, 0}}}, 0
};

static Coll_param fa_coll_param= {
  &fa_reorder_param, TRUE
};
#endif

/* Hungarian */
static const char hu_cldr_29[]=
#if 0
  /* Following rules are same as DUCET definition */
  "&C  <   cs      <<< Cs      <<< CS "
  "&D  <   dz      <<< Dz      <<< DZ "
  "&DZ <   dzs     <<< Dzs     <<< DZS "
  "&G  <   gy      <<< Gy      <<< GY "
  "&L  <   ly      <<< Ly      <<< LY "
  "&N  <   ny      <<< Ny      <<< NY "
  "&S  <   sz      <<< Sz      <<< SZ "
  "&T  <   ty      <<< Ty      <<< TY "
  "&Z  <   zs      <<< Zs      <<< ZS "
#endif
  "&O  <   \\u00F6 <<< \\u00D6 <<  \\u0151 <<< \\u0150 "
  "&U  <   \\u00FC <<< \\u00DC <<  \\u0171 <<< \\u0170 "
  "&cs <<< ccs/cs "
  "&Cs <<< Ccs/cs "
  "&CS <<< CCS/CS "
  "&dz <<< ddz/dz "
  "&Dz <<< Ddz/dz "
  "&DZ <<< DDZ/DZ "
  "&dzs<<< ddzs/dzs "
  "&Dzs<<< Ddzs/dzs "
  "&DZS<<< DDZS/DZS "
  "&gy <<< ggy/gy "
  "&Gy <<< Ggy/gy "
  "&GY <<< GGY/GY "
  "&ly <<< lly/ly "
  "&Ly <<< Lly/ly "
  "&LY <<< LLY/LY "
  "&ny <<< nny/ny "
  "&Ny <<< Nny/ny "
  "&NY <<< NNY/NY "
  "&sz <<< ssz/sz "
  "&Sz <<< Ssz/sz "
  "&SZ <<< SSZ/SZ "
  "&ty <<< tty/ty "
  "&Ty <<< Tty/ty "
  "&TY <<< TTY/TY "
  "&zs <<< zzs/zs "
  "&Zs <<< Zzs/zs "
  "&ZS <<< ZZS/ZS";

/* Croatian */
static const char hr_cldr_29[]=
  "&C <   \\u010D  <<< \\u010C <   \\u0107  <<< \\u0106 "
  "&D <   d\\u017E <<< \\u01C6 <<< D\\u017E <<< \\u01C5 <<< D\\u017D "
     "<<< \\u01C4  <   \\u0111 <<< \\u0110 "
  "&L <   lj       <<< \\u01C9 <<< Lj       <<< \\u01C8 <<< LJ "
     "<<< \\u01C7 "
  "&N <   nj       <<< \\u01CC <<< Nj       <<< \\u01CB <<< NJ "
     "<<< \\u01CA "
  "&S <   \\u0161  <<< \\u0160 "
  "&Z <   \\u017E  <<< \\u017D ";

static Reorder_param hr_reorder_param= {
  {CHARGRP_LATIN, CHARGRP_CYRILLIC, CHARGRP_NONE}, {{{0, 0}, {0, 0}}}, 0
};

static Coll_param hr_coll_param= {
  &hr_reorder_param, FALSE, CASE_FIRST_OFF
};

/* Sinhala */
#if 0
static const char si_cldr_29[]=
  "&\\u0D96 < \\u0D82 < \\u0D83 "
  "&\\u0DA5 < \\u0DA4";
#endif

/* Vietnamese */
static const char vi_cldr_29[]=
  "&\\u0300 << \\u0309 <<  \\u0303 << \\u0301 <<  \\u0323 "
  "&a       < \\u0103 <<< \\u0102 <  \\u00E2 <<< \\u00C2 "
  "&d       < \\u0111 <<< \\u0110 "
  "&e       < \\u00EA <<< \\u00CA "
  "&o       < \\u00F4 <<< \\u00D4 <  \\u01A1 <<< \\u01A0 "
  "&u       < \\u01B0 <<< \\u01AF";

static Coll_param vi_coll_param= {
  nullptr, TRUE, CASE_FIRST_OFF
};

static constexpr uint16 nochar[]= {0,0};

/**
  Unicode Collation Algorithm:
  Collation element (weight) scanner, 
  for consequent scan of collations
  weights from a string.

  Only meant as a base class; instantiate uca_scanner_any or uca_scanner_900
  instead of this.
*/
class my_uca_scanner
{
protected:
  my_uca_scanner(const CHARSET_INFO *cs_arg,
                 const MY_UCA_WEIGHT_LEVEL *level_arg,
                 const uchar *str, size_t length,
                 uint max_char_toscan_arg)
  : wbeg(nochar), sbeg(str), send(str + length), level(level_arg),
    cs(cs_arg), max_char_toscan(max_char_toscan_arg), sbeg_dup(str) {}

public:
  /**
    Get the level the scanner is currently working on. The string
    can be scanned multiple times (if the collation requires multi-level
    comparisons, e.g. for accent or case sensitivity); first to get
    primary weights, then from the start again for secondary, etc.
  */
  int get_weight_level() const { return weight_lv; }

  // TODO: These should be private.

  /**
    How many characters (possibly multibyte) we have scanned so far.
    This includes characters with zero weight. Note that this is reset
    once we get to the end of the string and restart the scanning for
    the next weight level.
  */
  uint char_index{0};

  /**
    The same as char_index, but counts only the first scan
    (for the primary level), not the successive levels.
  */
  int char_scanned{0};

protected:
  int weight_lv{0}; /* 0 = Primary, 1 = Secondary, 2 = Tertiary */
  const uint16 *wbeg;   /* Beginning of the current weight string */
  uint wbeg_stride{0};  /* Number of bytes between weights in string */
  const uchar  *sbeg;   /* Beginning of the input string          */
  const uchar  *send;   /* End of the input string                */
  const MY_UCA_WEIGHT_LEVEL *level;
  uint16 implicit[10];
  my_wc_t prev_char{0};  // Previous character we scanned, if any.
  const CHARSET_INFO *cs;
  int num_of_ce_left{0};
  uint max_char_toscan;  /* how many char's weight we want         */
  const uchar *sbeg_dup; /* Backup of beginning of input string */

protected:
  ALWAYS_INLINE(int next_implicit(my_wc_t ch));
  const uint16 *contraction_find(my_wc_t *wc);
  uint16 *previous_context_find(my_wc_t wc0, my_wc_t wc1);

  // FIXME: Should these just be a specialization in uca_scanner_900?
  void my_put_jamo_weights(my_wc_t *hangul_jamo, int jamo_cnt);
  ALWAYS_INLINE(int next_implicit_900(my_wc_t ch));
};

/*
  Charset dependent scanner part, to optimize
  some character sets.
*/

template<class Mb_wc>
struct uca_scanner_any : public my_uca_scanner
{
  uca_scanner_any(const Mb_wc mb_wc,
                  const CHARSET_INFO *cs,
                  const MY_UCA_WEIGHT_LEVEL *level,
                  const uchar *str, size_t length,
                  const uint max_char_toscan)
      : my_uca_scanner(cs, level, str, length, max_char_toscan),
        mb_wc(mb_wc) {
    // UCA 9.0.0 uses a different table format from what this scanner expects.
    DBUG_ASSERT(cs->uca == nullptr || cs->uca->version != UCA_V900);
  }

  ALWAYS_INLINE(int next());

private:
  const Mb_wc mb_wc;
};

template<class Mb_wc, int LEVELS_FOR_COMPARE>
class uca_scanner_900 : public my_uca_scanner
{
public:
  uca_scanner_900(const Mb_wc mb_wc,
                  const CHARSET_INFO *cs,
                  const MY_UCA_WEIGHT_LEVEL *level,
                  const uchar *str, size_t length,
                  const uint max_char_toscan)
      : my_uca_scanner(cs, level, str, length, max_char_toscan),
        mb_wc(mb_wc) {}

  ALWAYS_INLINE(int next());

  /**
    For each weight in sequence, call "func", which should have
    a function signature of "bool func(int weight)". Stops the
    iteration early if "func" returns false.

    This is morally equivalent to

      int weight;
      while ((weight= next()) >= 0)
      {
        if (!func(weight)) break;
      }

    except that it might employ optimizations internally to speed up
    the process. These optimizations will not modify the number of calls
    to func() (or their order), but might affect the internal scanner
    state during the calls, so func() should not try to read from
    the scanner except by calling public member functions.
  */
  template<class T>
  ALWAYS_INLINE(void for_each_weight(T func));

private:
  const Mb_wc mb_wc;

  ALWAYS_INLINE(int next_raw());
  ALWAYS_INLINE(int next_raw_single_level());
  inline int more_weight();
  uint16 apply_case_first(uint16 weight);
};


/********** Helper functions to handle contraction ************/


/**
  Mark a character as a contraction part
  
  @param list     Pointer to UCA data
  @param wc       Unicode code point
  @param flag     flag: "is contraction head", "is contraction tail"
*/

static inline void
my_uca_add_contraction_flag(MY_CONTRACTIONS *list, my_wc_t wc, int flag)
{
  list->flags[wc & MY_UCA_CNT_FLAG_MASK]|= flag;
}


/**
  Add a new contraction into contraction list
  
  @param list         Pointer to UCA data
  @param wc           Unicode code points of the characters
  @param len          Number of characters
  @param with_context Whether the comparison is context sensitive
  
  @return   New contraction
  @retval   Pointer to a newly added contraction
*/

static MY_CONTRACTION *
my_uca_add_contraction(MY_CONTRACTIONS *list, my_wc_t *wc, size_t len,
                       my_bool with_context)
{
  MY_CONTRACTION *next= &list->item[list->nitems];
  size_t i;
  /*
    Contraction is always at least 2 characters.
    Contraction is never longer than MY_UCA_MAX_CONTRACTION,
    which is guaranteed by using my_coll_rule_expand() with proper limit.
  */
  DBUG_ASSERT(len > 1 && len <= MY_UCA_MAX_CONTRACTION);
  for (i= 0; i < len; i++)
  {
    /*
      We don't support contractions with U+0000.
      my_coll_rule_expand() guarantees there're no U+0000 in a contraction.
    */
    DBUG_ASSERT(wc[i] != 0);
    next->ch[i]= wc[i];
  }
  if (i < MY_UCA_MAX_CONTRACTION)
    next->ch[i]= 0; /* Add end-of-line marker */
  next->with_context= with_context;
  list->nitems++;
  return next;
}


/**
  Allocate and initialize memory for contraction list and flags
  
  @param contractions      Pointer to UCA data
  @param loader            Pointer to charset loader
  @param n        Number of contractions
  
  @return   Error code
  @retval   0 - memory allocated successfully
  @retval   1 - not enough memory
*/

static my_bool
my_uca_alloc_contractions(MY_CONTRACTIONS *contractions,
                          MY_CHARSET_LOADER *loader, size_t n)
{
  size_t size= n * sizeof(MY_CONTRACTION);
  if (!(contractions->item= static_cast<MY_CONTRACTION*>((loader->once_alloc)(size))) ||
      !(contractions->flags= (char *) (loader->once_alloc)(MY_UCA_CNT_FLAG_SIZE)))
    return 1;
  memset(contractions->item, 0, size);
  memset(contractions->flags, 0, MY_UCA_CNT_FLAG_SIZE);
  return 0;
}


/**
  Return UCA contraction data for a CHARSET_INFO structure.

  @param cs       Pointer to CHARSET_INFO structure
  @param level    UCA comparison level
  @retval         Pointer to contraction data
  @retval         NULL, if this collation does not have UCA contraction
*/

const MY_CONTRACTIONS *
my_charset_get_contractions(const CHARSET_INFO *cs, int level)
{
  return (cs->uca != NULL) && (cs->uca->level[level].contractions.nitems > 0) ?
          &cs->uca->level[level].contractions : NULL;
}


/**
  Check if UCA level data has contractions.
  
  @param level    Pointer to UCA level data
  
  @return   Flags indicating if UCA with contractions
  @retval   0 - no contractions
  @retval   1 - there are some contractions
*/

static inline my_bool
my_uca_have_contractions(const MY_UCA_WEIGHT_LEVEL *level)
{
  return (level->contractions.nitems > 0);
}



/**
  Check if a character can be contraction head
  
  @param c        Pointer to UCA contraction data
  @param wc       Code point
  
  @retval   0 - cannot be contraction head
  @retval   1 - can be contraction head
*/

my_bool
my_uca_can_be_contraction_head(const MY_CONTRACTIONS *c, my_wc_t wc)
{
  return c->flags[wc & MY_UCA_CNT_FLAG_MASK] & MY_UCA_CNT_HEAD;
}


/**
  Check if a character can be contraction tail
  
  @param c        Pointer to UCA contraction data
  @param wc       Code point
  
  @retval   0 - cannot be contraction tail
  @retval   1 - can be contraction tail
*/

my_bool
my_uca_can_be_contraction_tail(const MY_CONTRACTIONS *c, my_wc_t wc)
{
  return c->flags[wc & MY_UCA_CNT_FLAG_MASK] & MY_UCA_CNT_TAIL;
}


/**
  Check if a character can be contraction part

  @param c        Pointer to UCA contraction data
  @param wc       Code point
  @param flag     UCA contraction flag

  @retval   0 - cannot be contraction part
  @retval   1 - can be contraction part
*/

static inline my_bool
my_uca_can_be_contraction_part(const MY_CONTRACTIONS *c, my_wc_t wc, int flag)
{
  return c->flags[wc & MY_UCA_CNT_FLAG_MASK] & flag;
}


/**
  Find a contraction consisting of two characters and return its weight array

  @param list     Pointer to UCA contraction data
  @param wc1      First character
  @param wc2      Second character

  @return   Weight array
  @retval   NULL - no contraction found
  @retval   ptr  - contraction weight array
*/

uint16 *
my_uca_contraction2_weight(const MY_CONTRACTIONS *list, my_wc_t wc1, my_wc_t wc2)
{
  MY_CONTRACTION *c, *last;
  for (c= list->item, last= c + list->nitems; c < last; c++)
  {
    if (c->ch[0] == wc1 && c->ch[1] == wc2 && c->ch[2] == 0)
    {
      return c->weight;
    }
  }
  return NULL;
}


/**
  Check if a character can be previous context head

  @param list     Pointer to UCA contraction data
  @param wc       Code point

  @return
  @retval   FALSE - cannot be previous context head
  @retval   TRUE  - can be previous context head
*/

static my_bool
my_uca_can_be_previous_context_head(const MY_CONTRACTIONS *list, my_wc_t wc)
{
  return list->flags[wc & MY_UCA_CNT_FLAG_MASK] & MY_UCA_PREVIOUS_CONTEXT_HEAD;
}


/**
  Check if a character can be previois context tail

  @param list     Pointer to UCA contraction data
  @param wc       Code point

  @return
  @retval   FALSE - cannot be contraction tail
  @retval   TRUE - can be contraction tail
*/

static my_bool
my_uca_can_be_previous_context_tail(const MY_CONTRACTIONS *list, my_wc_t wc)
{
  return list->flags[wc & MY_UCA_CNT_FLAG_MASK] & MY_UCA_PREVIOUS_CONTEXT_TAIL;
}


/**
  Compare two wide character strings, wide analog to strncmp().

  @param a      Pointer to the first string
  @param b      Pointer to the second string
  @param len    Length of the strings

  @return
  @retval       0 - strings are equal
  @retval       non-zero - strings are different
*/

static int
my_wmemcmp(const my_wc_t *a, const my_wc_t *b, size_t len)
{
  return memcmp(a, b, len * sizeof(my_wc_t));
}


/**
  Check if a string is a contraction of exactly the given length,
  and return its weight array on success.

  @param list   Pointer to UCA contraction data
  @param wc     Pointer to wide string
  @param len    String length

  @return       Weight array
  @retval       NULL - Input string is not a known contraction
  @retval       ptr  - contraction weight array
*/

static inline const uint16 *
my_uca_contraction_weight(const MY_CONTRACTIONS *list, const my_wc_t *wc, size_t len)
{
  MY_CONTRACTION *c, *last;
  for (c= list->item, last= c + list->nitems; c < last; c++)
  {
    if ((len == MY_UCA_MAX_CONTRACTION || c->ch[len] == 0) &&
        !c->with_context &&
        !my_wmemcmp(c->ch, wc, len))
      return c->weight;
  }
  return NULL;
}


/**
  Find a contraction in the input stream and return its weight array

  Scan input characters while their flags tell that they can be
  a contraction part. Then try to find real contraction among the
  candidates, starting from the longest.

  @param[inout] wc Where to store the scanned string. wc[0] is assumed
    already filled out with the first character (which should have the
    MY_UCA_CNT_HEAD flag).

  @return         Weight array
  @retval         NULL no contraction found
  @retval         ptr  contraction weight array
*/

const uint16 *
my_uca_scanner::contraction_find(my_wc_t *wc)
{
  size_t clen= 1;
  int flag;
  uchar *s, *beg[MY_UCA_MAX_CONTRACTION];
  memset(beg, 0, sizeof(beg));

  /*
    Find the length of the longest possible contraction starting from
    this point, by checking if the second character is the second character
    in any contraction (MY_UCA_CNT_MID1), the third character is the
    third character in any contraction, etc. This can easily yield false
    positives (e.g., if ABC is a contraction and DEF is also a contraction,
    we will return clen=3 here also for AE* -- the tail check isn't done
    before further down), but it helps us narrow down the maximum length
    efficiently.
  */
  for (s= (uchar*)sbeg, flag= MY_UCA_CNT_MID1;
       clen < MY_UCA_MAX_CONTRACTION;
       flag<<= 1)
  {
    int mblen;
    if ((mblen= cs->cset->mb_wc(cs, &wc[clen], s, send)) <= 0)
      break;
    s+= mblen;
    beg[clen]= s;

    /*
      NOTE: The test here will be bogus for maximum-length contractions
      (flag overflows into MY_UCA_PREVIOUS_CONTEXT_HEAD),
      but we'll be breaking anyway.
    */
    if (!my_uca_can_be_contraction_part(&level->contractions,
                                        wc[clen++], flag))
      break;
  }

  /* Find among candidates the longest real contraction */
  for ( ; clen > 1; clen--)
  {
    const uint16 *cweight;
    if (my_uca_can_be_contraction_tail(&level->contractions,
                                       wc[clen - 1]) &&
        (cweight= my_uca_contraction_weight(&level->contractions,
                                            wc, clen)))
    {
      if (cs->uca->version == UCA_V900)
      {
        cweight+= weight_lv;
        wbeg= cweight + MY_UCA_900_CE_SIZE;
        wbeg_stride= MY_UCA_900_CE_SIZE;
        num_of_ce_left= 7;
      }
      else
      {
        wbeg= cweight + 1;
        wbeg_stride= MY_UCA_900_CE_SIZE;
      }
      sbeg= beg[clen - 1];
      return cweight;
    }
  }

  return NULL; /* No contractions were found */
}


/**
  Find weight for contraction with previous context
  and return its weight array.

  @param wc0      Previous character
  @param wc1      Current character

  @return   Weight array
  @retval   NULL - no contraction with context found
  @retval   ptr  - contraction weight array
*/

uint16 *
my_uca_scanner::previous_context_find(my_wc_t wc0, my_wc_t wc1)
{
  const MY_CONTRACTIONS *list= &level->contractions;
  MY_CONTRACTION *c, *last;
  for (c= list->item, last= c + list->nitems; c < last; c++)
  {
    if (c->with_context && wc0 == c->ch[0] && wc1 == c->ch[1])
    {
      if (cs->uca->version == UCA_V900)
      {
        wbeg= c->weight + MY_UCA_900_CE_SIZE +
                       weight_lv;
        wbeg_stride= MY_UCA_900_CE_SIZE;
        num_of_ce_left= 7;
      }
      else
      {
        wbeg= c->weight + 1;
        wbeg_stride= MY_UCA_900_CE_SIZE;
      }
      return c->weight + weight_lv;
    }
  }
  return NULL;
}

/****************************************************************/
#define HANGUL_JAMO_MAX_LENGTH 3
/**
  Check if a character is Hangul syllable. Decompose it to jamos
  if it is, and return tailored weights.

  @param       syllable    Hangul syllable to be decomposed
  @param[out]  jamo        Corresponding jamos

  @return      0           The character is not Hangul syllable
                           or cannot be decomposed
               others      The number of jamos returned
*/
static int
my_decompose_hangul_syllable(my_wc_t syllable, my_wc_t* jamo)
{
  if (syllable < 0xAC00 || syllable > 0xD7AF)
    return 0;
  const int syllable_base= 0xAC00;
  const int leadingjamo_base= 0x1100;
  const int voweljamo_base= 0x1161;
  const int trailingjamo_base= 0x11A7;
  const int voweljamo_cnt= 21;
  const int trailingjamo_cnt= 28;
  int syllable_index= syllable - syllable_base;
  int v_t_combination= voweljamo_cnt * trailingjamo_cnt;
  int leadingjamo_index= syllable_index / v_t_combination;
  int voweljamo_index= (syllable_index % v_t_combination) / trailingjamo_cnt;
  int trailingjamo_index= syllable_index % trailingjamo_cnt;
  jamo[0]= leadingjamo_base + leadingjamo_index;
  jamo[1]= voweljamo_base + voweljamo_index;
  jamo[2]= trailingjamo_index ? (trailingjamo_base + trailingjamo_index) : 0;
  return trailingjamo_index ? 3 : 2;
}

void my_uca_scanner::my_put_jamo_weights(my_wc_t *hangul_jamo, int jamo_cnt)
{
  for (int jamoind= 0; jamoind < jamo_cnt; jamoind++)
  {
    uint16 *implicit_weight= implicit + jamoind * MY_UCA_900_CE_SIZE;
    int page= hangul_jamo[jamoind] >> 8;
    int code= hangul_jamo[jamoind] & 0xFF;
    const uint16 *jamo_weight_page= level->weights[page];
    implicit_weight[0]= UCA900_WEIGHT(jamo_weight_page, 0, code);
    implicit_weight[1]= UCA900_WEIGHT(jamo_weight_page, 1, code);
    implicit_weight[2]= UCA900_WEIGHT(jamo_weight_page, 2, code);
  }
  implicit[9]= jamo_cnt;
}

inline int my_uca_scanner::next_implicit_900(my_wc_t ch)
{
  my_wc_t hangul_jamo[HANGUL_JAMO_MAX_LENGTH];
  int jamo_cnt;
  if ((jamo_cnt= my_decompose_hangul_syllable(ch, hangul_jamo)))
  {
    my_put_jamo_weights(hangul_jamo, jamo_cnt);
    num_of_ce_left= jamo_cnt - 1;
    wbeg= implicit + MY_UCA_900_CE_SIZE + weight_lv;
    wbeg_stride= MY_UCA_900_CE_SIZE;
    return *(implicit + weight_lv);
  }

  int page;
  if (ch >= 0x17000 && ch <= 0x18AFF) //Tangut character
  {
    page= 0xFB00;
    implicit[3]= (ch - 0x17000) | 0x8000;
  }
  else
  {
    page= ch >> 15;
    implicit[3]= (ch & 0x7FFF) | 0x8000;
    if ((ch >= 0x3400 && ch <= 0x4DB5) ||
        (ch >= 0x20000 && ch <= 0x2A6D6) ||
        (ch >= 0x2A700 && ch <= 0x2B734) ||
        (ch >= 0x2B740 && ch <= 0x2B81D) ||
        (ch >= 0x2B820 && ch <= 0x2CEA1))
      page+= 0xFB80;
    else if ((ch >= 0x4E00 && ch <= 0x9FD5) ||
             (ch >= 0xFA0E && ch <= 0xFA29))
      page+= 0xFB40;
    else
      page+= 0xFBC0;
  }
  implicit[1]= 0x0020;
  implicit[2]= 0x0002;
  implicit[4]= 0;
  implicit[5]= 0;
  implicit[9]= 2;
  num_of_ce_left= 1;
  wbeg= implicit + MY_UCA_900_CE_SIZE + weight_lv;
  wbeg_stride= MY_UCA_900_CE_SIZE;
  implicit[0]= page;

  return *(implicit + weight_lv);
}

/**
  Return implicit UCA weight
  Used for characters that do not have assigned UCA weights.
  
  @return   The leading implicit weight.
*/

inline int
my_uca_scanner::next_implicit(my_wc_t ch)
{
  if (cs->uca->version == UCA_V900)
    return next_implicit_900(ch);

  implicit[0]= (ch & 0x7FFF) | 0x8000;
  implicit[1]= 0;
  wbeg= implicit;
  wbeg_stride= MY_UCA_900_CE_SIZE;

  int page= ch >> 15;

  if (ch >= 0x3400 && ch <= 0x4DB5)
    page+= 0xFB80;
  else if (ch >= 0x4E00 && ch <= 0x9FA5)
    page+= 0xFB40;
  else
    page+= 0xFBC0;

  return page;
}


template<class Mb_wc>
inline int uca_scanner_any<Mb_wc>::next()
{
  /* 
    Check if the weights for the previous character have been
    already fully scanned. If yes, then get the next character and 
    initialize wbeg and wlength to its weight string.
  */

  if (wbeg[0])      /* More weights left from the previous step: */
    return *wbeg++; /* return the next weight from expansion     */

  do
  {
    uint16 *wpage;
    my_wc_t wc[MY_UCA_MAX_CONTRACTION];
    int mblen;

    if (char_index >= max_char_toscan)
      return -1;
    /* Get next character */
    if (((mblen= mb_wc(wc, sbeg, send)) <= 0))
      return -1;

    sbeg+= mblen;
    char_index++;
    if (wc[0] > level->maxchar)
    {
      /* Return 0xFFFD as weight for all characters outside BMP */
      wbeg= nochar;
      wbeg_stride= 0;
      return 0xFFFD;
    }

    if (my_uca_have_contractions(level))
    {
      const uint16 *cweight;
      /*
        If we have scanned a character which can have previous context,
        and there were some more characters already before,
        then verify that {prev_char, wc[0]} together form
        a real previous context pair.
        Note, we support only 2-character long sequences with previous
        context at the moment. CLDR does not have longer sequences.
      */
      if (my_uca_can_be_previous_context_tail(&level->contractions, wc[0]) &&
          wbeg != nochar &&     /* if not the very first character */
          my_uca_can_be_previous_context_head(&level->contractions, prev_char) &&
          (cweight= previous_context_find(prev_char, wc[0])))
      {
        prev_char= 0; /* Clear for the next character */
        return *cweight;
      }
      else if (my_uca_can_be_contraction_head(&level->contractions,
                                              wc[0]))
      {
        /* Check if w[0] starts a contraction */
        if ((cweight= contraction_find(wc)))
          return *cweight;
      }
    }

    /* Process single character */
    prev_char= wc[0];
    int page= wc[0] >> 8;
    int code= wc[0] & 0xFF;

    /* If weight page for w[0] does not exist, then calculate algoritmically */
    if (!(wpage= level->weights[page]))
      return next_implicit(wc[0]);

    /* Calculate pointer to w[0]'s weight, using page and offset */
    wbeg= wpage + code * level->lengths[page];
    wbeg_stride= UCA900_DISTANCE_BETWEEN_WEIGHTS;
  } while (!wbeg[0]); /* Skip ignorable characters */

  return *wbeg++;
}

template<class Mb_wc, int LEVELS_FOR_COMPARE>
inline int uca_scanner_900<Mb_wc, LEVELS_FOR_COMPARE>::more_weight()
{
  /*
    Check if the weights for the previous character have been
    already fully scanned. If no, return the first non-zero
    weight.
  */

  while (num_of_ce_left != 0 && *wbeg == 0)
  {
    wbeg+= wbeg_stride;
    --num_of_ce_left;
  }
  if (num_of_ce_left != 0)
  {
    uint16 rtn= *wbeg;
    wbeg+= wbeg_stride;
    --num_of_ce_left;
    return rtn; /* return the next weight from expansion     */
  }
  return -1;
}

// Generic version that can handle any number of levels.
template<class Mb_wc, int LEVELS_FOR_COMPARE>
inline int uca_scanner_900<Mb_wc, LEVELS_FOR_COMPARE>::next_raw()
{
  if (LEVELS_FOR_COMPARE == 1)
  {
    return next_raw_single_level();
  }

  int remain_weight= more_weight();
  if (remain_weight >= 0)
    return remain_weight;

  do
  {
    my_wc_t wc[MY_UCA_MAX_CONTRACTION];
    int mblen= 0;

    /* Get next character */
    if (char_index >= max_char_toscan ||
        ((mblen= mb_wc(wc, sbeg, send)) <= 0))
    {
      sbeg= sbeg_dup;
      weight_lv++;
      char_index= 0;
      if (weight_lv < LEVELS_FOR_COMPARE)
        return 0; //Add level seperator
      return -1;
    }

    sbeg+= mblen;
    char_index++;
    if (weight_lv == 0)
      char_scanned++;
    if (wc[0] > level->maxchar)
    {
      /* Return 0xFFFD as weight for all characters outside BMP */
      wbeg= nochar;
      wbeg_stride= 0;
      num_of_ce_left= 0;
      weight_lv= 0;
      return 0xFFFD;
    }

    if (my_uca_have_contractions(level))
    {
      const uint16 *cweight;
      /*
        If we have scanned a character which can have previous context,
        and there were some more characters already before,
        then verify that {prev_char, wc[0]} together form
        a real previous context pair.
        Note, we support only 2-character long sequences with previous
        context at the moment. CLDR does not have longer sequences.
      */
      if (my_uca_can_be_previous_context_tail(&level->contractions, wc[0]) &&
          wbeg != nochar &&     /* if not the very first character */
          my_uca_can_be_previous_context_head(&level->contractions, prev_char) &&
          (cweight= previous_context_find(prev_char, wc[0])))
      {
        prev_char= 0; /* Clear for the next character */
        return *cweight;
      }
      else if (my_uca_can_be_contraction_head(&level->contractions,
                                              wc[0]))
      {
        /* Check if w[0] starts a contraction */
        if ((cweight= contraction_find(wc)))
          return *cweight;
      }
    }

    /* Process single character */
    prev_char= wc[0];
    int page= wc[0] >> 8;
    int code= wc[0] & 0xFF;

    /* If weight page for w[0] does not exist, then calculate algoritmically */
    const uint16 *wpage= level->weights[page];
    if (!wpage)
      return next_implicit(wc[0]);

    /* Calculate pointer to w[0]'s weight, using page and offset */
    wbeg= UCA900_WEIGHT_ADDR(wpage, weight_lv, code);
    wbeg_stride= UCA900_DISTANCE_BETWEEN_WEIGHTS;
    num_of_ce_left= UCA900_NUM_OF_CE(wpage, code);
  } while (!wbeg[0]); /* Skip ignorable characters */

  uint16 rtn= *wbeg;
  wbeg+= wbeg_stride;
  --num_of_ce_left;
  return rtn;
}

// Specialized, faster version for only one level.
template<class Mb_wc, int LEVELS_FOR_COMPARE>
inline int uca_scanner_900<Mb_wc, LEVELS_FOR_COMPARE>::next_raw_single_level()
{
  int remain_weight= more_weight();
  if (remain_weight >= 0)
    return remain_weight;

  do
  {
    my_wc_t wc[MY_UCA_MAX_CONTRACTION];
    int mblen= 0;

    /* Get next character */
    if (char_index >= max_char_toscan ||
        (mblen= mb_wc(wc, sbeg, send)) <= 0)
    {
      weight_lv++;
      return -1;
    }

    sbeg+= mblen;
    char_index++;
    char_scanned++;
    if (wc[0] > level->maxchar)
    {
      /* Return 0xFFFD as weight for all characters outside BMP */
      wbeg= nochar;
      wbeg_stride= 0;
      num_of_ce_left= 0;
      weight_lv= 0;
      return 0xFFFD;
    }

    if (my_uca_have_contractions(level))
    {
      const uint16 *cweight;
      /*
        If we have scanned a character which can have previous context,
        and there were some more characters already before,
        then verify that {prev_char, wc[0]} together form
        a real previous context pair.
        {prev_wc, wc[0]} together form a real previous context pair.
        Note, we support only 2-character long sequences with previous
        context at the moment. CLDR does not have longer sequences.
      */
      if (my_uca_can_be_previous_context_tail(&level->contractions, wc[0]) &&
          wbeg != nochar &&     /* if not the very first character */
          my_uca_can_be_previous_context_head(&level->contractions, prev_char) &&
          (cweight= previous_context_find(prev_char, wc[0])))
      {
        prev_char= 0; /* Clear for the next character */
        return *cweight;
      }
      if (my_uca_can_be_contraction_head(&level->contractions, wc[0]))
      {
        /* Check if w[0] starts a contraction */
        const uint16 *cweight= contraction_find(wc);
        if (cweight)
          return *cweight;
      }
    }

    /* Process single character */
    prev_char= wc[0];
    int page= wc[0] >> 8;
    int code= wc[0] & 0xFF;

    /* If weight page for w[0] does not exist, then calculate algoritmically */
    const uint16 *wpage= level->weights[page];
    if (!wpage)
      return next_implicit(wc[0]);

    /* Calculate pointer to w[0]'s weight, using page and offset */
    wbeg= UCA900_WEIGHT_ADDR(wpage, weight_lv, code);
    wbeg_stride= UCA900_DISTANCE_BETWEEN_WEIGHTS;
    num_of_ce_left= UCA900_NUM_OF_CE(wpage, code);
  } while (!wbeg[0]); /* Skip ignorable characters */

  uint16 rtn= *wbeg;
  wbeg+= wbeg_stride;
  --num_of_ce_left;
  return rtn;
}

template<class Mb_wc, int LEVELS_FOR_COMPARE>
template<class T>
inline void uca_scanner_900<Mb_wc, LEVELS_FOR_COMPARE>::for_each_weight(T func)
{
  if (cs->tailoring || cs->mbminlen != 1)
  {
    // Slower, generic path.
    int s_res;
    while ((s_res= next()) >= 0)
    {
      if (!func(s_res)) return;
    }
    return;
  }

  /*
    Fast path. TODO: See if we can accept some character sets
    with tailorings.
  */
  const uint16 *ascii_wpage= UCA900_WEIGHT_ADDR(
    cs->uca->level->weights[0], /*level=*/weight_lv, /*subcode=*/0);

  for ( ;; )
  {
    /*
      We could have more weights left from the previous call to next()
      (if any) that we need to deal with.
    */
    int s_res;
    while ((s_res= more_weight()) >= 0)
    {
      if (!func(s_res)) return;
    }

    /*
      Loop in a simple fast path as long as we only have ASCII characters.
      ASCII characters always have just a single weight and consist of
      only a single byte, so we can skip a lot of the checks we'd otherwise
      have to do.
    */
    const uchar *sbeg_copy= sbeg;
    const uchar *send_local=
      std::min(send, sbeg + (max_char_toscan - char_index));
    while (sbeg < send_local && *sbeg < 0x80)
    {
      const int s_res= ascii_wpage[*sbeg++];
      if (s_res && !func(s_res))
      {
        char_index+= sbeg - sbeg_copy;
        if (LEVELS_FOR_COMPARE == 1 || weight_lv == 0)
          char_scanned+= sbeg - sbeg_copy;
        return;
      }
    }
    char_index+= sbeg - sbeg_copy;
    if (LEVELS_FOR_COMPARE == 1 || weight_lv == 0)
      char_scanned+= sbeg - sbeg_copy;

    // Do a single character in the generic path.
    s_res= next();
    if (s_res == 0)
    {
      // Level separator, so we have to update our page pointer.
      ascii_wpage+= UCA900_DISTANCE_BETWEEN_LEVELS;
    }
    if (s_res < 0 || !func(s_res)) return;
  }
}

/**
  Change a weight according to the reorder parameters.
  @param   wt_rec     Weight boundary for each character group and gap
                      between groups
  @param   max_weight The maximum weight of char groups to reorder
  @param   weight     The weight to change
*/
static uint16
my_apply_reorder_param(const Reorder_wt_rec(&wt_rec)[2 * UCA_MAX_CHAR_GRP],
                       const int max_weight, uint16 weight)
{
  if (weight >= START_WEIGHT_TO_REORDER && weight <= max_weight)
  {
    for (int rec_ind= 0; rec_ind < 2 * UCA_MAX_CHAR_GRP; ++rec_ind)
    {
      if (wt_rec[rec_ind].old_wt_bdy.begin == 0 &&
          wt_rec[rec_ind].old_wt_bdy.end == 0)
        break;
      if (weight >= wt_rec[rec_ind].old_wt_bdy.begin &&
          weight <= wt_rec[rec_ind].old_wt_bdy.end)
      {
        return weight - wt_rec[rec_ind].old_wt_bdy.begin +
          wt_rec[rec_ind].new_wt_bdy.begin;
      }
    }
  }
  return weight;
}

// See Unicode TR35 section 3.14.1.
static bool is_tertiary_weight_upper_case(uint16 weight)
{
  if ((weight >= 0x08 && weight <= 0x0C) || weight == 0x0E ||
      weight == 0x11 || weight == 0x12 || weight == 0x1D)
    return true;
  return false;
}

template<class Mb_wc, int LEVELS_FOR_COMPARE>
uint16 uca_scanner_900<Mb_wc, LEVELS_FOR_COMPARE>::apply_case_first(
    uint16 weight)
{
  /*
    We only apply case weight change here when the character is not tailored.
    Tailored character's case weight has been changed in
    my_char_weight_put_900().
    We have only 1 collation (Danish) needs to implement [caseFirst upper].
  */
  if (cs->coll_param->case_first == CASE_FIRST_UPPER &&
      weight_lv == 2 && weight < 0x20)
  {
    if (is_tertiary_weight_upper_case(weight))
      weight|= CASE_FIRST_UPPER_MASK;
    else
      weight|= CASE_FIRST_LOWER_MASK;
  }
  return weight;
}

template<class Mb_wc, int LEVELS_FOR_COMPARE>
inline int uca_scanner_900<Mb_wc, LEVELS_FOR_COMPARE>::next()
{
  int res= next_raw();
  Coll_param *param= cs->coll_param;
  if (res > 0 && param)
  {
    /* Reorder weight change only on primary level. */
    if (param->reorder_param && weight_lv == 0)
      res= my_apply_reorder_param(param->reorder_param->wt_rec,
                                  param->reorder_param->max_weight,
                                  res);
    if (param->case_first != CASE_FIRST_OFF)
      res= apply_case_first(res);
  }
  return res;
}

/*
  Compares two strings according to the collation

  SYNOPSIS:
    my_strnncoll_uca()
    cs		Character set information
    s		First string
    slen	First string length
    t		Second string
    tlen	Seconf string length
  
  NOTES:
    Initializes two weight scanners and gets weights
    corresponding to two strings in a loop. If weights are not
    the same at some step then returns their difference.
    
    In the while() comparison these situations are possible:
    1. (s_res>0) and (t_res>0) and (s_res == t_res)
       Weights are the same so far, continue comparison
    2. (s_res>0) and (t_res>0) and (s_res!=t_res)
       A difference has been found, return.
    3. (s_res>0) and (t_res<0)
       We have reached the end of the second string, or found
       an illegal multibyte sequence in the second string.
       Return a positive number, i.e. the first string is bigger.
    4. (s_res<0) and (t_res>0)   
       We have reached the end of the first string, or found
       an illegal multibyte sequence in the first string.
       Return a negative number, i.e. the second string is bigger.
    5. (s_res<0) and (t_res<0)
       Both scanners returned -1. It means we have riched
       the end-of-string of illegal-sequence in both strings
       at the same time. Return 0, strings are equal.
    
  RETURN
    Difference between two strings, according to the collation:
    0               - means strings are equal
    negative number - means the first string is smaller
    positive number - means the first string is bigger
*/

template<class Scanner, class Mb_wc>
static int my_strnncoll_uca(const CHARSET_INFO *cs, 
                            const Mb_wc mb_wc,
			    const uchar *s, size_t slen,
                            const uchar *t, size_t tlen,
                            my_bool t_is_prefix)
{
  Scanner sscanner(mb_wc, cs, &cs->uca->level[0], s, slen, slen);
  Scanner tscanner(mb_wc, cs, &cs->uca->level[0], t, tlen, tlen);
  int s_res;
  int t_res;
  
  do
  {
    s_res= sscanner.next();
    t_res= tscanner.next();
  } while ( s_res == t_res && s_res >0);
  
  return  (t_is_prefix && t_res < 0) ? 0 : (s_res - t_res);
}


static inline int
my_space_weight(const CHARSET_INFO *cs) /* W3-TODO */
{
  if (cs->uca && cs->uca->version == UCA_V900)
    return UCA900_WEIGHT(
      cs->uca->level[0].weights[0], /*weight_lv=*/0, 0x20);
  else
    return cs->uca->level[0].weights[0][0x20 * cs->uca->level[0].lengths[0]];
}


/**
  Helper function:
  Find address of weights of the given character.

  @param level    Pointer to UCA level data
  @param wc       character Unicode code point

  @return Weight array
    @retval  pointer to weight array for the given character,
             or nullptr if this page does not have implicit weights.
*/

static inline uint16 *
my_char_weight_addr(MY_UCA_WEIGHT_LEVEL *level, my_wc_t wc)
{
  uint page, ofst;
  return wc > level->maxchar ? nullptr :
         (level->weights[page= (wc >> 8)] ?
          level->weights[page] + (ofst= (wc & 0xFF)) * level->lengths[page] :
          nullptr);
}

/**
  Helper function:
  Find address of weights of the given character, for UCA 9.0.0 format.

  @param level    Pointer to UCA level data
  @param wc       character Unicode code point

  @return Weight array
    @retval  pointer to weight array for the given character,
             or nullptr if this page does not have implicit weights.
*/

static inline uint16 *
my_char_weight_addr_900(MY_UCA_WEIGHT_LEVEL *level, my_wc_t wc)
{
  if (wc > level->maxchar)
    return nullptr;

  uint page= wc >> 8;
  uint ofst= wc & 0xFF;
  uint16 *weights= level->weights[page];
  if (weights)
    return UCA900_WEIGHT_ADDR(weights, /*level=*/0, ofst);
  else
    return nullptr;
}


/*
  Compares two strings according to the collation,
  ignoring trailing spaces.

  SYNOPSIS:
    my_strnncollsp_uca()
    cs		Character set information
    s		First string
    slen	First string length
    t		Second string
    tlen	Second string length
  
  NOTES:
    Works exactly the same with my_strnncoll_uca(),
    but ignores trailing spaces.

    In the while() comparison these situations are possible:
    1. (s_res>0) and (t_res>0) and (s_res == t_res)
       Weights are the same so far, continue comparison
    2. (s_res>0) and (t_res>0) and (s_res!=t_res)
       A difference has been found, return.
    3. (s_res>0) and (t_res<0)
       We have reached the end of the second string, or found
       an illegal multibyte sequence in the second string.
       Compare the first string to an infinite array of
       space characters until difference is found, or until
       the end of the first string.
    4. (s_res<0) and (t_res>0)   
       We have reached the end of the first string, or found
       an illegal multibyte sequence in the first string.
       Compare the second string to an infinite array of
       space characters until difference is found or until
       the end of the second steing.
    5. (s_res<0) and (t_res<0)
       Both scanners returned -1. It means we have riched
       the end-of-string of illegal-sequence in both strings
       at the same time. Return 0, strings are equal.
  
  RETURN
    Difference between two strings, according to the collation:
    0               - means strings are equal
    negative number - means the first string is smaller
    positive number - means the first string is bigger
*/

template<class Mb_wc>
static int my_strnncollsp_uca(const CHARSET_INFO *cs, 
                              Mb_wc mb_wc,
                              const uchar *s, size_t slen,
                              const uchar *t, size_t tlen)
{
  int s_res, t_res;
  
  uca_scanner_any<Mb_wc> sscanner(mb_wc, cs, &cs->uca->level[0], s, slen, slen);
  uca_scanner_any<Mb_wc> tscanner(mb_wc, cs, &cs->uca->level[0], t, tlen, tlen);

  do
  {
    s_res= sscanner.next();
    t_res= tscanner.next();
  } while ( s_res == t_res && s_res >0);

  if (s_res > 0 && t_res < 0)
  { 
    /* Calculate weight for SPACE character */
    t_res= my_space_weight(cs);
      
    /* compare the first string to spaces */
    do
    {
      if (s_res != t_res)
        return (s_res - t_res);
      s_res= sscanner.next();
    } while (s_res > 0);
    return 0;
  }
    
  if (s_res < 0 && t_res > 0)
  {
    /* Calculate weight for SPACE character */
    s_res= my_space_weight(cs);
      
    /* compare the second string to spaces */
    do
    {
      if (s_res != t_res)
        return (s_res - t_res);
      t_res= tscanner.next();
    } while (t_res > 0);
    return 0;
  }
  
  return ( s_res - t_res );
}

template<class Mb_wc, int LEVELS_FOR_COMPARE>
static int my_strnncollsp_uca_900_tmpl(const CHARSET_INFO *cs,
                                       const Mb_wc mb_wc,
                                       const uchar *s, size_t slen,
                                       const uchar *t, size_t tlen)
{
  int s_res= 0;
  int t_res= 0;

  uca_scanner_900<Mb_wc, LEVELS_FOR_COMPARE> sscanner(
    mb_wc, cs, &cs->uca->level[0], s, slen, slen);
  uca_scanner_900<Mb_wc, LEVELS_FOR_COMPARE> tscanner(
    mb_wc, cs, &cs->uca->level[0], t, tlen, tlen);

  /*
    We compare 2 strings in same level first. If only string A's scanner
    has gone to next level, which means another string, B's weight of
    current level is longer than A's. We'll compare B's remaining weights
    with space.
  */
  for (int current_lv= 0; current_lv < LEVELS_FOR_COMPARE; ++current_lv)
  {
    /* Run the scanners until one of them runs out of current lv */
    do
    {
      s_res= sscanner.next();
      t_res= tscanner.next();
    } while (s_res == t_res && s_res >= 0 &&
             sscanner.get_weight_level() == current_lv &&
             tscanner.get_weight_level() == current_lv);
    /* Two scanners run to next level at same time */
    if (sscanner.get_weight_level() == tscanner.get_weight_level())
    {
      if (s_res == t_res && s_res >= 0)
        continue;
      break;
    }

    if (tscanner.get_weight_level() > current_lv)
    {
      const uint16 space_weight= UCA900_WEIGHT(
        cs->uca->level[0].weights[0], current_lv, 0x20);
      /* compare the first string to spaces */
      do
      {
        if (s_res != space_weight)
          return (s_res - space_weight);
        s_res= sscanner.next();
      } while (s_res >= 0 && sscanner.get_weight_level() == current_lv);
      if (sscanner.get_weight_level() > current_lv && s_res == t_res)
        continue;
      break;
    }

    if (sscanner.get_weight_level() > current_lv)
    {
      const uint16 space_weight= UCA900_WEIGHT(
        cs->uca->level[0].weights[0], current_lv, 0x20);
      /* compare the second string to spaces */
      do
      {
        if (space_weight != t_res)
          return (space_weight - t_res);
        t_res= tscanner.next();
      } while (t_res >= 0 && tscanner.get_weight_level() == current_lv);
      if (tscanner.get_weight_level() > current_lv && s_res == t_res)
        continue;
      break;
    }
  }

  return ( s_res - t_res );
}

// Simpler version of my_strnncollsp_uca_900_tmpl for only a single level.
// (Avoids calling get_weight_level(), which saves time.)
template<class Mb_wc>
static int my_strnncollsp_uca_900_tmpl_single_level(const CHARSET_INFO *cs,
                                                    const Mb_wc mb_wc,
                                                    const uchar *s, size_t slen,
                                                    const uchar *t, size_t tlen)
{
  int s_res= 0;
  int t_res= 0;

  uca_scanner_900<Mb_wc, 1> sscanner(
    mb_wc, cs, &cs->uca->level[0], s, slen, slen);
  uca_scanner_900<Mb_wc, 1> tscanner(
    mb_wc, cs, &cs->uca->level[0], t, tlen, tlen);

  do
  {
    s_res= sscanner.next();
    t_res= tscanner.next();
  } while (s_res == t_res && s_res >= 0);

  if (s_res != t_res && s_res >= 0 && t_res >= 0)
  {
    // Most common case: Both scanners still have weights left,
    // but they are different.
    return ( s_res - t_res );
  }

  if (s_res < 0 && t_res < 0)
  {
    // Both ended at the same time, without ever showing a difference.
    return 0;
  }

  uint16 space_weight= UCA900_WEIGHT(
    cs->uca->level[0].weights[0], /*weight_lv=*/0, 0x20);

  if (t_res < 0)
  {
    /* compare the first string to spaces */
    do
    {
      if (s_res != space_weight)
        return (s_res - space_weight);
      s_res= sscanner.next();
    } while (s_res >= 0);

    // The first string ran out without showing anything but spaces.
    return 0;
  }

  DBUG_ASSERT(s_res < 0);

  /* compare the second string to spaces */
  do
  {
    if (space_weight != t_res)
      return (space_weight - t_res);
    t_res= tscanner.next();
  } while (t_res >= 0);

  // The second string ran out without showing anything but spaces.
  return 0;
}


static int my_strnncollsp_uca_900(const CHARSET_INFO *cs,
                                  const uchar *s, size_t slen,
                                  const uchar *t, size_t tlen)
{
  if (cs->cset->mb_wc == my_mb_wc_utf8mb4_thunk)
  {
    switch (cs->levels_for_compare) {
    case 1:
      return my_strnncollsp_uca_900_tmpl_single_level<Mb_wc_utf8mb4>(
        cs, Mb_wc_utf8mb4(), s, slen, t, tlen);
    case 2:
      return my_strnncollsp_uca_900_tmpl<Mb_wc_utf8mb4, 2>(
        cs, Mb_wc_utf8mb4(), s, slen, t, tlen);
    default:
      DBUG_ASSERT(false);
    case 3:
      return my_strnncollsp_uca_900_tmpl<Mb_wc_utf8mb4, 3>(
        cs, Mb_wc_utf8mb4(), s, slen, t, tlen);
    }
  }
  else
  {
    Mb_wc_through_function_pointer mb_wc(cs);
    switch (cs->levels_for_compare)
    {
    case 1:
      return my_strnncollsp_uca_900_tmpl_single_level<decltype(mb_wc)>(
        cs, mb_wc, s, slen, t, tlen);
    case 2:
      return my_strnncollsp_uca_900_tmpl<decltype(mb_wc), 2>(
        cs, mb_wc, s, slen, t, tlen);
    default:
      DBUG_ASSERT(false);
    case 3:
      return my_strnncollsp_uca_900_tmpl<decltype(mb_wc), 3>(
        cs, mb_wc, s, slen, t, tlen);
    }
  }
}

/*
  Calculates hash value for the given string,
  according to the collation, and ignoring trailing spaces.
  
  SYNOPSIS:
    my_hash_sort_uca()
    cs		Character set information
    s		String
    slen	String's length
    n1		First hash parameter
    n2		Second hash parameter
  
  NOTES:
    Scans consequently weights and updates
    hash parameters n1 and n2. In a case insensitive collation,
    upper and lower case of the same letter will return the same
    weight sequence, and thus will produce the same hash values
    in n1 and n2.
  
  RETURN
    N/A
*/

template<class Mb_wc>
static void my_hash_sort_uca(const CHARSET_INFO *cs,
                             Mb_wc mb_wc,
			     const uchar *s, size_t slen,
			     ulong *n1, ulong *n2)
{
  int   s_res;
  ulong tmp1;
  ulong tmp2;

  slen= cs->cset->lengthsp(cs, (char*) s, slen);
  uca_scanner_any<Mb_wc> scanner(mb_wc, cs, &cs->uca->level[0], s, slen, slen);

  tmp1= *n1;
  tmp2= *n2;

  while ((s_res= scanner.next()) >0)
  {
    tmp1^= (((tmp1 & 63) + tmp2) * (s_res >> 8))+ (tmp1 << 8);
    tmp2+=3;
    tmp1^= (((tmp1 & 63) + tmp2) * (s_res & 0xFF))+ (tmp1 << 8);
    tmp2+=3;
  }

  *n1= tmp1;
  *n2= tmp2;
}


/*
  For the given string creates its "binary image", suitable
  to be used in binary comparison, i.e. in memcmp(). 
  
  SYNOPSIS:
    my_strnxfrm_uca()
    cs		Character set information
    dst		Where to write the image
    dstlen	Space available for the image, in bytes
    src		The source string
    srclen	Length of the source string, in bytes
  
  NOTES:
    In a loop, scans weights from the source string and writes
    them into the binary image. In a case insensitive collation,
    upper and lower cases of the same letter will produce the
    same image subsequences. When we have reached the end-of-string
    or found an illegal multibyte sequence, the loop stops.

    It is impossible to restore the original string using its
    binary image. 
    
    Binary images are used for bulk comparison purposes,
    e.g. in ORDER BY, when it is more efficient to create
    a binary image and use it instead of weight scanner
    for the original strings for every comparison.
  
  RETURN
    Number of bytes that have been written into the binary image.
*/


template<class Mb_wc>
static size_t
my_strnxfrm_uca(const CHARSET_INFO *cs, Mb_wc mb_wc,
                uchar *dst, size_t dstlen, uint nweights,
                const uchar *src, size_t srclen, uint flags)
{
  uchar *d0= dst;
  uchar *de= dst + dstlen;
  int   s_res;
  uca_scanner_any<Mb_wc> scanner(
    mb_wc, cs, &cs->uca->level[0], src, srclen, nweights);
  
  while (dst < de && (s_res= scanner.next()) > 0)
  {
    *dst++= s_res >> 8;
    if (dst < de)
      *dst++= s_res & 0xFF;
  }
  nweights-= scanner.char_index;
  
  if (dst < de && nweights && (flags & MY_STRXFRM_PAD_WITH_SPACE))
  {
    uint space_count= MY_MIN((uint) (de - dst) / 2, nweights);
    s_res= my_space_weight(cs);
    for (; space_count ; space_count--)
    {
      *dst++= s_res >> 8;
      *dst++= s_res & 0xFF;
    }
  }
  my_strxfrm_desc_and_reverse(d0, dst, flags, 0);
  if ((flags & MY_STRXFRM_PAD_TO_MAXLEN) && dst < de)
  {
    s_res= my_space_weight(cs);
    for ( ; dst < de; )
    {
      *dst++= s_res >> 8;
      if (dst < de)
        *dst++= s_res & 0xFF;
    }
  }
  return dst - d0;
}


static int my_uca_charcmp_900(const CHARSET_INFO *cs, my_wc_t wc1, my_wc_t wc2)
{
  uint16 *weight1= my_char_weight_addr_900(&cs->uca->level[0], wc1); /* W3-TODO */
  uint16 *weight2= my_char_weight_addr_900(&cs->uca->level[0], wc2);

  /* Check if some of the characters does not have implicit weights */
  if (!weight1 || !weight2)
    return wc1 != wc2;

  /* Quickly compare first weights */
  if (weight1[0] != weight2[0])
    return 1;

  /* Thoroughly compare all weights */
  size_t length1= weight1[-UCA900_DISTANCE_BETWEEN_LEVELS];
  size_t length2= weight2[-UCA900_DISTANCE_BETWEEN_LEVELS];

  if (length1 != length2)
    return 1;

  if (cs->state & MY_CS_CSSORT)
  {
    for (size_t weightind= 0; weightind < length1 * MY_UCA_900_CE_SIZE;
         ++weightind)
    {
      if (*weight1 != *weight2)
        return 1;
      weight1+= UCA900_DISTANCE_BETWEEN_LEVELS;
      weight2+= UCA900_DISTANCE_BETWEEN_LEVELS;
    }
  }
  else
  {
    for (size_t weightind= 0; weightind < length1; ++weightind)
    {
      if (*weight1 != *weight2)
        return 1;
      weight1+= UCA900_DISTANCE_BETWEEN_WEIGHTS;
      weight2+= UCA900_DISTANCE_BETWEEN_WEIGHTS;
    }
  }

  return 0;
}


/*
  This function compares if two characters are the same.
  The sign +1 or -1 does not matter. The only
  important thing is that the result is 0 or not 0.
  This fact allows us to use memcmp() safely, on both
  little-endian and big-endian machines.
*/

static int my_uca_charcmp(const CHARSET_INFO *cs, my_wc_t wc1, my_wc_t wc2)
{
  if (wc1 == wc2)
    return 0;

  if (cs->uca != nullptr && cs->uca->version == UCA_V900)
    return my_uca_charcmp_900(cs, wc1, wc2);

  size_t length1, length2;
  uint16 *weight1= my_char_weight_addr(&cs->uca->level[0], wc1); /* W3-TODO */
  uint16 *weight2= my_char_weight_addr(&cs->uca->level[0], wc2);

  /* Check if some of the characters does not have implicit weights */
  if (!weight1 || !weight2)
    return wc1 != wc2;

  /* Quickly compare first weights */
  if (weight1[0] != weight2[0])
    return 1;

  /* Thoroughly compare all weights */
  length1= cs->uca->level[0].lengths[wc1 >> MY_UCA_PSHIFT]; /* W3-TODO */
  length2= cs->uca->level[0].lengths[wc2 >> MY_UCA_PSHIFT];

  if (length1 > length2)
    return memcmp((const void*)weight1, (const void*)weight2, length2*2) ?
           1: weight1[length2];

  if (length1 < length2)
    return memcmp((const void*)weight1, (const void*)weight2, length1*2) ?
           1 : weight2[length1];

  return memcmp((const void*)weight1, (const void*)weight2, length1*2);
}

/*** Compare string against string with wildcard
**	0 if matched
**	-1 if not matched with wildcard
**	 1 if matched with wildcard
*/

static
int my_wildcmp_uca_impl(const CHARSET_INFO *cs,
                        const char *str,const char *str_end,
                        const char *wildstr,const char *wildend,
                        int escape, int w_one, int w_many, int recurse_level)
{
  int result= -1;			/* Not found, using wildcards */
  my_wc_t s_wc, w_wc;
  int scan;
  int (*mb_wc)(const struct charset_info_st *, my_wc_t *,
               const uchar *, const uchar *);
  mb_wc= cs->cset->mb_wc;

 if (my_string_stack_guard && my_string_stack_guard(recurse_level))
   return 1;
  while (wildstr != wildend)
  {
    while (1)
    {
      my_bool escaped= 0;
      if ((scan= mb_wc(cs, &w_wc, (const uchar*)wildstr,
		       (const uchar*)wildend)) <= 0)
	return 1;

      if (w_wc == (my_wc_t)w_many)
      {
        result= 1;				/* Found an anchor char */
        break;
      }

      wildstr+= scan;
      if (w_wc ==  (my_wc_t)escape)
      {
        if ((scan= mb_wc(cs, &w_wc, (const uchar*)wildstr,
			(const uchar*)wildend)) <= 0)
          return 1;
        wildstr+= scan;
        escaped= 1;
      }
      
      if ((scan= mb_wc(cs, &s_wc, (const uchar*)str,
      		       (const uchar*)str_end)) <= 0)
        return 1;
      str+= scan;
      
      if (!escaped && w_wc == (my_wc_t)w_one)
      {
        result= 1;				/* Found an anchor char */
      }
      else
      {
        if (my_uca_charcmp(cs, s_wc, w_wc))
          return 1;
      }
      if (wildstr == wildend)
	return (str != str_end);		/* Match if both are at end */
    }
    
    
    if (w_wc == (my_wc_t)w_many)
    {						/* Found w_many */
    
      /* Remove any '%' and '_' from the wild search string */
      for ( ; wildstr != wildend ; )
      {
        if ((scan= mb_wc(cs, &w_wc, (const uchar*)wildstr,
			 (const uchar*)wildend)) <= 0)
          return 1;
        
	if (w_wc == (my_wc_t)w_many)
	{
	  wildstr+= scan;
	  continue;
	} 
	
	if (w_wc == (my_wc_t)w_one)
	{
	  wildstr+= scan;
	  if ((scan= mb_wc(cs, &s_wc, (const uchar*)str,
			   (const uchar*)str_end)) <= 0)
            return 1;
          str+= scan;
	  continue;
	}
	break;					/* Not a wild character */
      }
      
      if (wildstr == wildend)
	return 0;				/* Ok if w_many is last */
      
      if (str == str_end)
	return -1;
      
      if ((scan= mb_wc(cs, &w_wc, (const uchar*)wildstr,
		       (const uchar*)wildend)) <= 0)
        return 1;
      
      if (w_wc ==  (my_wc_t)escape)
      {
        wildstr+= scan;
        if ((scan= mb_wc(cs, &w_wc, (const uchar*)wildstr,
			 (const uchar*)wildend)) <= 0)
          return 1;
      }
      
      while (1)
      {
        /* Skip until the first character from wildstr is found */
        while (str != str_end)
        {
          if ((scan= mb_wc(cs, &s_wc, (const uchar*)str,
			   (const uchar*)str_end)) <= 0)
            return 1;
          
          if (!my_uca_charcmp(cs, s_wc, w_wc))
            break;
          str+= scan;
        }
        if (str == str_end)
          return -1;
        
        result= my_wildcmp_uca_impl(cs, str, str_end, wildstr, wildend,
                                    escape, w_one, w_many, recurse_level + 1);
        
        if (result <= 0)
          return result;
        
        str+= scan;
      } 
    }
  }
  return (str != str_end ? 1 : 0);
}


static
int my_strcasecmp_uca(const CHARSET_INFO *cs, const char *s, const char *t)
{
  const MY_UNICASE_INFO *uni_plane= cs->caseinfo;
  const MY_UNICASE_CHARACTER *page;
  while (s[0] && t[0])
  {
    my_wc_t s_wc, t_wc;

    if (static_cast<uchar>(s[0]) < 128)
    {
      s_wc= uni_plane->page[0][static_cast<uchar>(s[0])].tolower;
      s++;
    }
    else
    {
      int res;

      res= cs->cset->mb_wc(cs, &s_wc, pointer_cast<const uchar*>(s),
                           pointer_cast<const uchar*>(s + 4));

      if (res <= 0)
        return strcmp(s, t);
      s+= res;
      if (s_wc <= uni_plane->maxchar && (page = uni_plane->page[s_wc >> 8]))
        s_wc= page[s_wc & 0xFF].tolower;
    }


    /* Do the same for the second string */

    if (static_cast<uchar>(t[0]) < 128)
    {
      /* Convert single byte character into weight */
      t_wc= uni_plane->page[0][static_cast<uchar>(t[0])].tolower;
      t++;
    }
    else
    {
      int res= cs->cset->mb_wc(cs, &t_wc, pointer_cast<const uchar*>(t),
		               pointer_cast<const uchar*>(t + 4));
      if (res <= 0)
        return strcmp(s, t);
      t+= res;

      if (t_wc <= uni_plane->maxchar && (page = uni_plane->page[t_wc >> 8]))
        t_wc= page[t_wc & 0xFF].tolower;
    }

    /* Now we have two weights, let's compare them */
    if (s_wc != t_wc)
      return static_cast<int>(s_wc) - static_cast<int>(t_wc);
  }
  return static_cast<int>(static_cast<uchar>(s[0])) -
         static_cast<int>(static_cast<uchar>(t[0]));
}


extern "C" {
static int my_wildcmp_uca(const CHARSET_INFO *cs,
                          const char *str,const char *str_end,
                          const char *wildstr,const char *wildend,
                          int escape, int w_one, int w_many)
{
  return my_wildcmp_uca_impl(cs, str, str_end,
                             wildstr, wildend,
                             escape, w_one, w_many, 1);
}
} // extern "C"


/*
  Collation language is implemented according to
  subset of ICU Collation Customization (tailorings):
  http://icu.sourceforge.net/userguide/Collate_Customization.html
  
  Collation language elements:
  Delimiters:
    space   - skipped
  
  <char> :=  A-Z | a-z | \uXXXX
  
  Shift command:
    <shift>  := &       - reset at this letter. 
  
  Diff command:
    <d1> :=  <     - Identifies a primary difference.
    <d2> :=  <<    - Identifies a secondary difference.
    <d3> := <<<    - Idenfifies a tertiary difference.
  
  
  Collation rules:
    <ruleset> :=  <rule>  { <ruleset> }
    
    <rule> :=   <d1>    <string>
              | <d2>    <string>
              | <d3>    <string>
              | <shift> <char>
    
    <string> := <char> [ <string> ]

  An example, Polish collation:
  
    &A < \u0105 <<< \u0104
    &C < \u0107 <<< \u0106
    &E < \u0119 <<< \u0118
    &L < \u0142 <<< \u0141
    &N < \u0144 <<< \u0143
    &O < \u00F3 <<< \u00D3
    &S < \u015B <<< \u015A
    &Z < \u017A <<< \u017B    
*/


typedef enum my_coll_lexem_num_en
{
  MY_COLL_LEXEM_EOF     = 0,
  MY_COLL_LEXEM_SHIFT   = 1,
  MY_COLL_LEXEM_RESET   = 4,
  MY_COLL_LEXEM_CHAR    = 5,
  MY_COLL_LEXEM_ERROR   = 6,
  MY_COLL_LEXEM_OPTION  = 7,
  MY_COLL_LEXEM_EXTEND  = 8,
  MY_COLL_LEXEM_CONTEXT = 9
} my_coll_lexem_num;


/**
  Convert collation customization lexem to string,
  for nice error reporting

  @param term   lexem code

  @return       lexem name
*/

static const char *
my_coll_lexem_num_to_str(my_coll_lexem_num term)
{
  switch (term)
  {
  case MY_COLL_LEXEM_EOF:    return "EOF";
  case MY_COLL_LEXEM_SHIFT:  return "Shift";
  case MY_COLL_LEXEM_RESET:  return "&";
  case MY_COLL_LEXEM_CHAR:   return "Character";
  case MY_COLL_LEXEM_OPTION: return "Bracket option";
  case MY_COLL_LEXEM_EXTEND: return "/";
  case MY_COLL_LEXEM_CONTEXT:return "|";
  case MY_COLL_LEXEM_ERROR:  return "ERROR";
  }
  return NULL;
}


typedef struct my_coll_lexem_st
{
  my_coll_lexem_num term;
  const char *beg;
  const char *end;
  const char *prev;
  int   diff;
  int   code;
} MY_COLL_LEXEM;


/*
  Initialize collation rule lexical anilizer
  
  SYNOPSIS
    my_coll_lexem_init
    lexem                Lex analizer to init
    str                  Const string to parse
    str_end               End of the string
  USAGE
  
  RETURN VALUES
    N/A
*/

static void my_coll_lexem_init(MY_COLL_LEXEM *lexem,
                               const char *str, const char *str_end)
{
  lexem->beg= str;
  lexem->prev= str;
  lexem->end= str_end;
  lexem->diff= 0;
  lexem->code= 0;
}


/**
  Compare lexem to string with length

  @param lexem       lexem
  @param pattern     string
  @param patternlen  string length

  @return
  @retval            0 if lexem is equal to string, non-0 otherwise.
*/

static int
lex_cmp(MY_COLL_LEXEM *lexem, const char *pattern, size_t patternlen)
{
  size_t lexemlen= lexem->beg - lexem->prev;
  if (lexemlen < patternlen)
    return 1; /* Not a prefix */
  return native_strncasecmp(lexem->prev, pattern, patternlen);
}


/*
  Print collation customization expression parse error, with context.
  
  SYNOPSIS
    my_coll_lexem_print_error
    lexem                Lex analizer to take context from
    errstr               sting to write error to
    errsize              errstr size
    txt                  error message
    col_name             collation name
  USAGE
  
  RETURN VALUES
    N/A
*/

static void my_coll_lexem_print_error(MY_COLL_LEXEM *lexem,
                                      char *errstr, size_t errsize,
                                      const char *txt, const char *col_name)
{
  char tail[30];
  size_t len= lexem->end - lexem->prev;
  strmake (tail, lexem->prev, (size_t) MY_MIN(len, sizeof(tail)-1));
  errstr[errsize-1]= '\0';
  my_snprintf(errstr, errsize - 1,
              "%s at '%s' for COLLATION : %s", txt[0] ? txt : "Syntax error",
              tail, col_name);
}


/*
  Convert a hex digit into its numeric value
  
  SYNOPSIS
    ch2x
    ch                   hex digit to convert
  USAGE
  
  RETURN VALUES
    an integer value in the range 0..15
    -1 on error
*/

static int ch2x(int ch)
{
  if (ch >= '0' && ch <= '9')
    return ch - '0';
  
  if (ch >= 'a' && ch <= 'f')
    return 10 + ch - 'a';
  
  if (ch >= 'A' && ch <= 'F')
    return 10 + ch - 'A';
  
  return -1;
}


/*
  Collation language lexical parser:
  Scans the next lexem.
  
  SYNOPSIS
    my_coll_lexem_next
    lexem                Lex analizer, previously initialized by 
                         my_coll_lexem_init.
  USAGE
    Call this function in a loop
    
  RETURN VALUES
    Lexem number: eof, diff, shift, char or error.
*/

static my_coll_lexem_num my_coll_lexem_next(MY_COLL_LEXEM *lexem)
{
  const char *beg;
  my_coll_lexem_num rc;

  for (beg= lexem->beg ; beg < lexem->end ; beg++)
  {
    switch (*beg)
    {
    case ' ':
    case '\t':
    case '\r':
    case '\n':
      continue;

    case '[':  /* Bracket expression, e.g. "[optimize [a-z]]" */
      {
        size_t nbrackets; /* Indicates nested recursion level */
        for (beg++, nbrackets= 1 ; beg < lexem->end; beg++)
        {
          if (*beg == '[') /* Enter nested bracket expression */
            nbrackets++;
          else if (*beg == ']')
          {
            if (--nbrackets == 0)
            {
              rc= MY_COLL_LEXEM_OPTION;
              beg++;
              goto ex;
            }
          }
        }
        rc= MY_COLL_LEXEM_ERROR;
        goto ex;
      }

    case '&':
      beg++;
      rc= MY_COLL_LEXEM_RESET;
      goto ex;

    case '=':
      beg++;
      lexem->diff= 0;
      rc= MY_COLL_LEXEM_SHIFT;
      goto ex;

    case '/':
      beg++;
      rc= MY_COLL_LEXEM_EXTEND;
      goto ex;

    case '|':
      beg++;
      rc= MY_COLL_LEXEM_CONTEXT;
      goto ex;

    case '<':   /* Shift: '<' or '<<' or '<<<' or '<<<<' */
      {
        /* Scan up to 3 additional '<' characters */
        for (beg++, lexem->diff= 1;
             (beg < lexem->end) && (*beg == '<') && (lexem->diff <= 3);
             beg++, lexem->diff++);
        rc= MY_COLL_LEXEM_SHIFT;
        goto ex;
      }
      default:
        break;
    }

    /* Escaped character, e.g. \u1234 */
    if ((*beg == '\\') && (beg + 2 < lexem->end) &&
        (beg[1] == 'u') && my_isxdigit(&my_charset_utf8_general_ci, beg[2]))
    {
      int ch;
      
      beg+= 2;
      lexem->code= 0;
      while ((beg < lexem->end) && ((ch= ch2x(beg[0])) >= 0))
      { 
        lexem->code= (lexem->code << 4) + ch;
        beg++;
      }
      rc= MY_COLL_LEXEM_CHAR;
      goto ex;
    }

    /*
      Unescaped single byte character:
        allow printable ASCII range except SPACE and
        special characters parsed above []<&/|=
    */
    if (*beg >= 0x21 && *beg <= 0x7E)
    {
      lexem->code= *beg++;
      rc= MY_COLL_LEXEM_CHAR;
      goto ex;
    }

    if (((uchar) *beg) > 0x7F) /* Unescaped multibyte character */
    {
      CHARSET_INFO *cs= &my_charset_utf8_general_ci;
      my_wc_t wc;
      int nbytes= cs->cset->mb_wc(cs, &wc,
                                  (uchar *) beg, (uchar *) lexem->end);
      if (nbytes > 0)
      {
        rc= MY_COLL_LEXEM_CHAR;
        beg+= nbytes;
        lexem->code= (int) wc;
        goto ex;
      }
    }

    rc= MY_COLL_LEXEM_ERROR;
    goto ex;
  }
  rc= MY_COLL_LEXEM_EOF;

ex:
  lexem->prev= lexem->beg;
  lexem->beg= beg;
  lexem->term= rc;
  return rc;  
}


/*
  Collation rule item
*/

#define MY_UCA_MAX_EXPANSION  6  /* Maximum expansion length   */

typedef struct my_coll_rule_item_st
{
  my_wc_t base[MY_UCA_MAX_EXPANSION];    /* Base character                  */
  my_wc_t curr[MY_UCA_MAX_CONTRACTION];  /* Current character               */
  int diff[4];      /* Primary, Secondary, Tertiary, Quaternary difference  */
  size_t before_level;                   /* "reset before" indicator        */
  my_bool with_context;
} MY_COLL_RULE;


/**
  Return length of a 0-terminated wide string, analog to strnlen().

  @param  s       Pointer to wide string
  @param  maxlen  Mamixum string length

  @return         string length, or maxlen if no '\0' is met.
*/
static size_t
my_wstrnlen(my_wc_t *s, size_t maxlen)
{
  size_t i;
  for (i= 0; i < maxlen; i++)
  {
    if (s[i] == 0)
      return i;
  }
  return maxlen;
}


/**
  Return length of the "reset" string of a rule.

  @param  r  Collation customization rule

  @return    Length of r->base
*/

static inline size_t
my_coll_rule_reset_length(MY_COLL_RULE *r)
{
  return my_wstrnlen(r->base, MY_UCA_MAX_EXPANSION);
}


/**
  Return length of the "shift" string of a rule.

  @param  r  Collation customization rule

  @return    Length of r->base
*/

static inline size_t
my_coll_rule_shift_length(MY_COLL_RULE *r)
{
  return my_wstrnlen(r->curr, MY_UCA_MAX_CONTRACTION);
}


/**
  Append new character to the end of a 0-terminated wide string.

  @param  wc     Wide string
  @param  limit  Maximum possible result length
  @param  code   Character to add

  @return        1 if character was added, 0 if string was too long
*/

static int
my_coll_rule_expand(my_wc_t *wc, size_t limit, my_wc_t code)
{
  size_t i;
  for (i= 0; i < limit; i++)
  {
    if (wc[i] == 0)
    {
      wc[i]= code;
      return 1;
    }
  }
  return 0;
}


/**
  Initialize collation customization rule

  @param  r     Rule
*/

static void
my_coll_rule_reset(MY_COLL_RULE *r)
{
  memset(r, 0, sizeof(*r));
}


/*
  Shift methods:
  Simple: "&B < C" : weight('C') = weight('B') + 1
  Expand: weight('C') =  { weight('B'), weight(last_non_ignorable) + 1 }
*/
typedef enum
{
  my_shift_method_simple= 0,
  my_shift_method_expand
} my_coll_shift_method;


typedef struct my_coll_rules_st
{
  MY_UCA_INFO *uca;          /* Unicode weight data               */
  size_t nrules;             /* Number of rules in the rule array */
  size_t mrules;             /* Number of allocated rules         */
  MY_COLL_RULE *rule;        /* Rule array                        */
  MY_CHARSET_LOADER *loader;
  my_coll_shift_method shift_after_method;
} MY_COLL_RULES;


/**
  Realloc rule array to a new size.
  Reallocate memory for 128 additional rules at once,
  to reduce the number of reallocs, which is important
  for long tailorings (e.g. for East Asian collations).

  @param  rules   Rule container
  @param  n       new number of rules

  @return         0 on success, -1 on error.
*/

static int
my_coll_rules_realloc(MY_COLL_RULES *rules, size_t n)
{
  if (rules->nrules < rules->mrules ||
      (rules->rule=
       static_cast<MY_COLL_RULE*>(rules->loader->mem_realloc(rules->rule,
                                                             sizeof(MY_COLL_RULE) *
                                                             (rules->mrules= n + 128)))))
    return 0;
  return -1;
}


/**
  Append one new rule to a rule array

  @param  rules   Rule container
  @param  rule    New rule to add

  @return         0 on success, -1 on error.
*/

static int
my_coll_rules_add(MY_COLL_RULES *rules, MY_COLL_RULE *rule)
{
  if (my_coll_rules_realloc(rules, rules->nrules + 1))
    return -1;
  rules->rule[rules->nrules++]= rule[0];
  return 0;
}


/**
  Apply difference at level

  @param  r      Rule
  @param  level  Level (0,1,2,3,4)
*/

static void
my_coll_rule_shift_at_level(MY_COLL_RULE *r, int level)
{
  switch (level)
  {
  case 4: /* Quaternary difference */
    r->diff[3]++;
    break;
  case 3: /* Tertiary difference */
    r->diff[2]++;
    r->diff[3]= 0;
    break;
  case 2: /* Secondary difference */
    r->diff[1]++;
    r->diff[2]= r->diff[3]= 0;
    break;
  case 1: /* Primary difference */
    r->diff[0]++;
    r->diff[1]= r->diff[2]= r->diff[3]= 0;
    break;
  case 0:
    /* Do nothing for '=': use the previous offsets for all levels */
    break;
  default:
    DBUG_ASSERT(0);
  }
}


typedef struct my_coll_rule_parser_st
{
  MY_COLL_LEXEM tok[2]; /* Current token and next token for look-ahead */
  MY_COLL_RULE rule;    /* Currently parsed rule */
  MY_COLL_RULES *rules; /* Rule list pointer     */
  char errstr[128];     /* Error message         */
} MY_COLL_RULE_PARSER;


/**
  Current parser token

  @param  p   Collation customization parser

  @return     Pointer to the current token
*/

static MY_COLL_LEXEM *
my_coll_parser_curr(MY_COLL_RULE_PARSER *p)
{
  return &p->tok[0];
}


/**
  Next parser token, to look ahead.

  @param  p   Collation customization parser

  @return     Pointer to the next token
*/

static MY_COLL_LEXEM *
my_coll_parser_next(MY_COLL_RULE_PARSER *p)
{
  return &p->tok[1];
}


/**
  Scan one token from the input stream

  @param  p   Collation customization parser

  @return     1, for convenience, to use in logical expressions easier.
*/
static int
my_coll_parser_scan(MY_COLL_RULE_PARSER *p)
{
  my_coll_parser_curr(p)[0]= my_coll_parser_next(p)[0];
  my_coll_lexem_next(my_coll_parser_next(p));
  return 1;
}


/**
  Initialize collation customization parser

  @param  p        Collation customization parser
  @param  rules    Where to store rules
  @param  str      Beginning of a collation customization sting
  @param  str_end  End of the collation customizations string
*/

static void
my_coll_parser_init(MY_COLL_RULE_PARSER *p,
                    MY_COLL_RULES *rules,
                    const char *str, const char *str_end)
{
  /*
    Initialize parser to the input buffer and scan two tokens,
    to make the current token and the next token known.
  */
  memset(p, 0, sizeof(*p));
  p->rules= rules;
  p->errstr[0]= '\0';
  my_coll_lexem_init(my_coll_parser_curr(p), str, str_end);
  my_coll_lexem_next(my_coll_parser_curr(p));
  my_coll_parser_next(p)[0]= my_coll_parser_curr(p)[0];
  my_coll_lexem_next(my_coll_parser_next(p));
}


/**
  Display error when an unexpected token found

  @param  p        Collation customization parser
  @param  term     Which lexem was expected

  @return          0, to use in "return" and boolean expressions.
*/

static int
my_coll_parser_expected_error(MY_COLL_RULE_PARSER *p, my_coll_lexem_num term)
{
  my_snprintf(p->errstr, sizeof(p->errstr),
              "%s expected", my_coll_lexem_num_to_str(term));
  return 0;
}


/**
  Display error when a too long character sequence is met

  @param  p        Collation customization parser
  @param  name     Which kind of sequence: contraction, expansion, etc.

  @return          0, to use in "return" and boolean expressions.
*/

static int
my_coll_parser_too_long_error(MY_COLL_RULE_PARSER *p, const char *name)
{
  my_snprintf(p->errstr, sizeof(p->errstr), "%s is too long", name);
  return 0;
}


/**
  Scan the given lexem from input stream, or display "expected" error.

  @param  p        Collation customization parser
  @param  term     Which lexem is expected.

  @return
  @retval          0 if the required term was not found.
  @retval          1 if the required term was found.
*/
static int
my_coll_parser_scan_term(MY_COLL_RULE_PARSER *p, my_coll_lexem_num term)
{
  if (my_coll_parser_curr(p)->term != term)
    return my_coll_parser_expected_error(p, term);
  return my_coll_parser_scan(p);
}


/*
  In the following code we have a few functions to parse
  various collation customization non-terminal symbols.
  Unlike our usual coding convension, they return
  - 0 on "error" (when the rule was not scanned) and
  - 1 on "success"(when the rule was scanned).
  This is done intentionally to make body of the functions look easier
  and repeat the grammar of the rules in straightforward manner.
  For example:

  // <x> ::= <y> | <z>
  int parse_x() { return parse_y() || parser_z(); }

  // <x> ::= <y> <z>
  int parse_x() { return parse_y() && parser_z(); }

  Using 1 on "not found" and 0 on "found" in the parser code would
  make the code more error prone and harder to read because
  of having to use inverse boolean logic.
*/


/**
  Scan a collation setting in brakets, for example UCA version.

  @param  p        Collation customization parser

  @return
  @retval          0 if setting was scanned.
  @retval          1 if setting was not scanned.
*/

static int
my_coll_parser_scan_setting(MY_COLL_RULE_PARSER *p)
{
  MY_COLL_RULES *rules= p->rules;
  MY_COLL_LEXEM *lexem= my_coll_parser_curr(p);

  if (!lex_cmp(lexem, C_STRING_WITH_LEN("[version 4.0.0]")))
  {
    rules->uca= &my_uca_v400;
  }
  else if (!lex_cmp(lexem, C_STRING_WITH_LEN("[version 5.2.0]")))
  {
    rules->uca= &my_uca_v520;
  }
  else if (!lex_cmp(lexem, C_STRING_WITH_LEN("[shift-after-method expand]")))
  {
    rules->shift_after_method= my_shift_method_expand;
  }
  else if (!lex_cmp(lexem, C_STRING_WITH_LEN("[shift-after-method simple]")))
  {
    rules->shift_after_method= my_shift_method_simple;
  }
  else
  {
    return 0;
  }
  return my_coll_parser_scan(p);
}


/**
  Scan multiple collation settings

  @param  p        Collation customization parser

  @return
  @retval          0 if no settings were scanned.
  @retval          1 if one or more settings were scanned.
*/

static int
my_coll_parser_scan_settings(MY_COLL_RULE_PARSER *p)
{
  /* Scan collation setting or special purpose command */
  while (my_coll_parser_curr(p)->term == MY_COLL_LEXEM_OPTION)
  {
    if (!my_coll_parser_scan_setting(p))
      return 0;
  }
  return 1;
}


/**
  Scan [before xxx] reset option

  @param  p        Collation customization parser

  @return
  @retval          0 if reset option was not scanned.
  @retval          1 if reset option was scanned.
*/

static int
my_coll_parser_scan_reset_before(MY_COLL_RULE_PARSER *p)
{
  MY_COLL_LEXEM *lexem= my_coll_parser_curr(p);
  if (!lex_cmp(lexem, C_STRING_WITH_LEN("[before primary]")) ||
      !lex_cmp(lexem, C_STRING_WITH_LEN("[before 1]")))
  {
    p->rule.before_level= 1;
  }
  else if (!lex_cmp(lexem, C_STRING_WITH_LEN("[before secondary]")) ||
           !lex_cmp(lexem, C_STRING_WITH_LEN("[before 2]")))
  {
    p->rule.before_level= 2;
  }
  else if (!lex_cmp(lexem, C_STRING_WITH_LEN("[before tertiary]")) ||
           !lex_cmp(lexem, C_STRING_WITH_LEN("[before 3]")))
  {
    p->rule.before_level= 3;
  }
  else if (!lex_cmp(lexem, C_STRING_WITH_LEN("[before quaternary]")) ||
           !lex_cmp(lexem, C_STRING_WITH_LEN("[before 4]")))
  {
    p->rule.before_level= 4;
  }
  else
  {
    p->rule.before_level= 0;
    return 0; /* Don't scan thr next character */
  }
  return my_coll_parser_scan(p);
}


/**
  Scan logical position and add to the wide string.

  @param  p        Collation customization parser
  @param  pwc      Wide string to add code to
  @param  limit    The result string cannot be longer than 'limit' characters

  @return
  @retval          0 if logical position was not scanned.
  @retval          1 if logical position was scanned.
*/

static int
my_coll_parser_scan_logical_position(MY_COLL_RULE_PARSER *p,
                                     my_wc_t *pwc, size_t limit)
{
  MY_COLL_RULES *rules= p->rules;
  MY_COLL_LEXEM *lexem= my_coll_parser_curr(p);

  if (!lex_cmp(lexem, C_STRING_WITH_LEN("[first non-ignorable]")))
    lexem->code= rules->uca->first_non_ignorable;
  else if (!lex_cmp(lexem, C_STRING_WITH_LEN("[last non-ignorable]")))
    lexem->code= rules->uca->last_non_ignorable;
  else if (!lex_cmp(lexem, C_STRING_WITH_LEN("[first primary ignorable]")))
    lexem->code= rules->uca->first_primary_ignorable;
  else if (!lex_cmp(lexem, C_STRING_WITH_LEN("[last primary ignorable]")))
    lexem->code= rules->uca->last_primary_ignorable;
  else if (!lex_cmp(lexem, C_STRING_WITH_LEN("[first secondary ignorable]")))
    lexem->code= rules->uca->first_secondary_ignorable;
  else if (!lex_cmp(lexem, C_STRING_WITH_LEN("[last secondary ignorable]")))
    lexem->code= rules->uca->last_secondary_ignorable;
  else if (!lex_cmp(lexem, C_STRING_WITH_LEN("[first tertiary ignorable]")))
    lexem->code= rules->uca->first_tertiary_ignorable;
  else if (!lex_cmp(lexem, C_STRING_WITH_LEN("[last tertiary ignorable]")))
    lexem->code= rules->uca->last_tertiary_ignorable;
  else if (!lex_cmp(lexem, C_STRING_WITH_LEN("[first trailing]")))
    lexem->code= rules->uca->first_trailing;
  else if (!lex_cmp(lexem, C_STRING_WITH_LEN("[last trailing]")))
    lexem->code= rules->uca->last_trailing;
  else if (!lex_cmp(lexem, C_STRING_WITH_LEN("[first variable]")))
    lexem->code= rules->uca->first_variable;
  else if (!lex_cmp(lexem, C_STRING_WITH_LEN("[last variable]")))
    lexem->code= rules->uca->last_variable;
  else
    return 0; /* Don't scan the next token */

  if (!my_coll_rule_expand(pwc, limit, lexem->code))
  {
    /*
      Logical position can not be in a contraction,
      so the above call should never fail.
      Let's assert in debug version and print
      a nice error message in production version.
    */
    DBUG_ASSERT(0);
    return my_coll_parser_too_long_error(p, "Logical position");
  }
  return my_coll_parser_scan(p);
}


/**
  Scan character list

    @<character list@> ::= CHAR [ CHAR... ]

  @param  p        Collation customization parser
  @param  pwc      Character string to add code to
  @param  limit    The result string cannot be longer than 'limit' characters
  @param  name     E.g. "contraction", "expansion"

  @return
  @retval          0 if character sequence was not scanned.
  @retval          1 if character sequence was scanned.
*/

static int
my_coll_parser_scan_character_list(MY_COLL_RULE_PARSER *p,
                                   my_wc_t *pwc, size_t limit,
                                   const char *name)
{
  if (my_coll_parser_curr(p)->term != MY_COLL_LEXEM_CHAR)
    return my_coll_parser_expected_error(p, MY_COLL_LEXEM_CHAR);

  if (!my_coll_rule_expand(pwc, limit, my_coll_parser_curr(p)->code))
    return my_coll_parser_too_long_error(p, name);

  if (!my_coll_parser_scan_term(p, MY_COLL_LEXEM_CHAR))
    return 0;

  while (my_coll_parser_curr(p)->term == MY_COLL_LEXEM_CHAR)
  {
    if (!my_coll_rule_expand(pwc, limit, my_coll_parser_curr(p)->code))
      return my_coll_parser_too_long_error(p, name);
    my_coll_parser_scan(p);
  }
  return 1;
}


/**
  Scan reset sequence

  @<reset sequence@> ::=
    [ @<reset before option@> ] @<character list@>
  | [ @<reset before option@> ] @<logical reset position@>

  @param  p        Collation customization parser

  @return
  @retval          0 if reset sequence was not scanned.
  @retval          1 if reset sequence was scanned.
*/

static int
my_coll_parser_scan_reset_sequence(MY_COLL_RULE_PARSER *p)
{
  my_coll_rule_reset(&p->rule);

  /* Scan "[before x]" option, if exists */
  if (my_coll_parser_curr(p)->term == MY_COLL_LEXEM_OPTION)
    my_coll_parser_scan_reset_before(p);

  /* Try logical reset position */
  if (my_coll_parser_curr(p)->term == MY_COLL_LEXEM_OPTION)
  {
    if (!my_coll_parser_scan_logical_position(p, p->rule.base, 1))
      return 0;
  }
  else
  {
    /* Scan single reset character or expansion */
    if (!my_coll_parser_scan_character_list(p, p->rule.base,
                                            MY_UCA_MAX_EXPANSION, "Expansion"))
      return 0;
  }

  if ((p->rules->shift_after_method == my_shift_method_expand ||
       p->rule.before_level == 1) &&
      p->rules->uca->version < UCA_V900)/* Apply "before primary" option  */
  {
    /*
      Suppose we have this rule:  &B[before primary] < C
      i.e. we need to put C before B, but after A, so
      the result order is: A < C < B.

      Let primary weight of B be [BBBB].

      We cannot just use [BBBB-1] as weight for C:
      DUCET does not have enough unused weights between any two characters,
      so using [BBBB-1] will likely make C equal to the previous character,
      which is A, so we'll get this order instead of the desired: A = C < B.

      To guarantee that that C is sorted after A, we'll use expansion
      with a kind of "biggest possible character".
      As "biggest possible character" we'll use "last_non_ignorable":

      We'll compose weight for C as: [BBBB-1][MMMM+1]
      where [MMMM] is weight for "last_non_ignorable".
      
      We also do the same trick for "reset after" if the collation
      option says so. E.g. for the rules "&B < C", weight for
      C will be calculated as: [BBBB][MMMM+1]

      At this point we only need to store codepoints
      'B' and 'last_non_ignorable'. Actual weights for 'C'
      will be calculated according to the above formula later,
      in create_tailoring().
    */
    if (!my_coll_rule_expand(p->rule.base, MY_UCA_MAX_EXPANSION,
                             p->rules->uca->last_non_ignorable))
      return my_coll_parser_too_long_error(p, "Expansion");
  }
  return 1;
}


/**
  Scan shift sequence

  @<shift sequence@> ::=
    @<character list@>  [ / @<character list@> ]
  | @<character list@>  [ | @<character list@> ]

  @param  p        Collation customization parser

  @return
  @retval          0 if shift sequence was not scanned.
  @retval          1 if shift sequence was scanned.
*/

static int
my_coll_parser_scan_shift_sequence(MY_COLL_RULE_PARSER *p)
{
  MY_COLL_RULE before_extend;

  memset(&p->rule.curr, 0, sizeof(p->rule.curr));

  /* Scan single shift character or contraction */
  if (!my_coll_parser_scan_character_list(p, p->rule.curr,
                                          MY_UCA_MAX_CONTRACTION,
                                          "Contraction"))
    return 0;

  before_extend= p->rule; /* Remember the part before "/" */

  /* Append the part after "/" as expansion */
  if (my_coll_parser_curr(p)->term == MY_COLL_LEXEM_EXTEND)
  {
    my_coll_parser_scan(p);
    if (!my_coll_parser_scan_character_list(p, p->rule.base,
                                            MY_UCA_MAX_EXPANSION,
                                            "Expansion"))
      return 0;
  }
  else if (my_coll_parser_curr(p)->term == MY_COLL_LEXEM_CONTEXT)
  {
    /*
      We support 2-character long context sequences only:
      one character is the previous context, plus the current character.
      It's OK as Unicode's CLDR does not have longer examples.
    */
    my_coll_parser_scan(p);
    p->rule.with_context= TRUE;
    if (!my_coll_parser_scan_character_list(p, p->rule.curr + 1, 1, "context"))
      return 0;
  }

  /* Add rule to the rule list */
  if (my_coll_rules_add(p->rules, &p->rule))
    return 0;

  p->rule= before_extend; /* Restore to the state before "/" */

  return 1;
}


/**
  Scan shift operator

  @<shift@> ::=  <  | <<  | <<<  | <<<<  | =

  @param  p        Collation customization parser

  @return
  @retval          0 if shift operator was not scanned.
  @retval          1 if shift operator was scanned.
*/
static int
my_coll_parser_scan_shift(MY_COLL_RULE_PARSER *p)
{
  if (my_coll_parser_curr(p)->term == MY_COLL_LEXEM_SHIFT)
  {
    my_coll_rule_shift_at_level(&p->rule, my_coll_parser_curr(p)->diff);
    return my_coll_parser_scan(p);
  }
  return 0;
}


/**
  Scan one rule: reset followed by a number of shifts

  @<rule@> ::=
    & @<reset sequence@>
    @<shift@> @<shift sequence@>
    [ { @<shift@> @<shift sequence@> }... ]

  @param  p        Collation customization parser

  @return
  @retval          0 if rule was not scanned.
  @retval          1 if rule was scanned.
*/
static int
my_coll_parser_scan_rule(MY_COLL_RULE_PARSER *p)
{
  if (!my_coll_parser_scan_term(p, MY_COLL_LEXEM_RESET) ||
      !my_coll_parser_scan_reset_sequence(p))
    return 0;

  /* Scan the first required shift command */
  if (!my_coll_parser_scan_shift(p))
    return my_coll_parser_expected_error(p, MY_COLL_LEXEM_SHIFT);

  /* Scan the first shift sequence */
  if (!my_coll_parser_scan_shift_sequence(p))
    return 0;

  /* Scan subsequent shift rules */
  while (my_coll_parser_scan_shift(p))
  {
    if (!my_coll_parser_scan_shift_sequence(p))
      return 0;
  }
  return 1;
}


/**
  Scan collation customization: settings followed by rules

  @<collation customization@> ::=
    [ @<setting@> ... ]
    [ @<rule@>... ]

  @param  p        Collation customization parser

  @return
  @retval          0 if collation customization expression was not scanned.
  @retval          1 if collation customization expression was scanned.
*/

static int
my_coll_parser_exec(MY_COLL_RULE_PARSER *p)
{
  if (!my_coll_parser_scan_settings(p))
    return 0;

  while (my_coll_parser_curr(p)->term == MY_COLL_LEXEM_RESET)
  {
    if (!my_coll_parser_scan_rule(p))
      return 0;
  }
  /* Make sure no unparsed input data left */
  return my_coll_parser_scan_term(p, MY_COLL_LEXEM_EOF);
}


/*
  Collation language syntax parser.
  Uses lexical parser.

  @param rules           Collation rule list to load to.
  @param str             A string with collation customization.
  @param str_end         End of the string.
  @param col_name        Collation name

  @return
  @retval                0 on success
  @retval                1 on error
*/

static int
my_coll_rule_parse(MY_COLL_RULES *rules,
                   const char *str, const char *str_end, const char *col_name)
{
  MY_COLL_RULE_PARSER p;

  my_coll_parser_init(&p, rules, str, str_end);

  if (!my_coll_parser_exec(&p))
  {
    my_coll_lexem_print_error(my_coll_parser_curr(&p),
                              rules->loader->error,
                              sizeof(rules->loader->error) - 1,
                              p.errstr, col_name);
    return 1;
  }
  return 0;
}

static void
spread_case_mask(uint16 *to, size_t to_stride, size_t tailored_ce_cnt,
                 uint16 case_mask)
{
  for (size_t i= 0; i < tailored_ce_cnt; ++i)
  {
    uint16 *case_weight= &to[(i * MY_UCA_900_CE_SIZE + 2) * to_stride];
    if (*case_weight > CASE_FIRST_UPPER_MASK)
      case_mask= *case_weight & 0xFF00;
    else if (*case_weight)
      *case_weight|= case_mask;
  }
}

/*
 If the collation is marked as [caseFirst upper], move all of the weights
 around to accomodate that. Only tailored weights are changed; for non-tailored
 weights, we do it on-the-fly in uca_scanner_900::apply_case_first().

 [caseFirst upper] is a directive that says that case should override all
 other tertiary case concerns (in a sense, a “level 2.5”), and furthermore,
 that uppercase should come before lowercase. (Normally lowercase sorts
 before uppercase.) It is currently only used in the Danish collation.

 This is done by looking at the tertiary weight, inferring the case from it,
 and then using the upper bits (which are normally unused) to signal the case.
 The algorithm is detailed in Unicode TR35, section 3.14, although we don't
 seem to follow it exactly.
*/
static void change_weight_if_case_first(CHARSET_INFO *cs, MY_COLL_RULE *r,
                                        uint16 *to, size_t to_stride,
                                        size_t curr_len, size_t tailored_ce_cnt)
{
  /* We only need to implement [caseFirst upper] right now. */
  if (!(cs->coll_param &&
        cs->coll_param->case_first == CASE_FIRST_UPPER &&
        cs->levels_for_compare == 3))
    return;

  DBUG_ASSERT(cs->uca->version == UCA_V900);

  // How many CEs this character has with non-ignorable primary weight.
  int tailored_pri_cnt= 0;
  int origin_pri_cnt= 0;
  for (size_t i= 0; i < tailored_ce_cnt; ++i)
  {
    /*
      If rule A has already applied a case weight change, and we have rule B
      which is inherited from A, apply the same case weight change on the rest
      of rule B and return.
    */
    if (to[(i * MY_UCA_900_CE_SIZE + 2) * to_stride] > CASE_FIRST_UPPER_MASK)
    {
      spread_case_mask(to, to_stride, tailored_ce_cnt, /*case_mask=*/0);
      return;
    }
    if (to[i * MY_UCA_900_CE_SIZE * to_stride])
      tailored_pri_cnt++;
  }
  if (r->before_level == 1 || r->diff[0])
    tailored_pri_cnt--;

  // Use the DUCET weight to detect the character's case.
  MY_UCA_WEIGHT_LEVEL *src= my_uca_v900.level;
  int changed_ce= 0;

  my_wc_t *curr= r->curr;
  for (size_t i= 0; i < curr_len; ++i)
  {
    const uint16 *from= my_char_weight_addr_900(src, *curr);
    int page= *curr >> 8;
    int code= *curr & 0xFF;
    curr++;
    int ce_cnt= src->weights[page] ? UCA900_NUM_OF_CE(src->weights[page], code) : 0;
    for (int i_ce= 0; i_ce < ce_cnt; ++i_ce)
    {
      if (from[i_ce * UCA900_DISTANCE_BETWEEN_WEIGHTS])
        origin_pri_cnt++;
    }
  }
  int case_to_copy= 0;
  if (origin_pri_cnt <= tailored_pri_cnt)
    case_to_copy= origin_pri_cnt;
  else
    case_to_copy= tailored_pri_cnt - 1;
  int upper_cnt= 0;
  int lower_cnt= 0;
  curr= r->curr;
  uint16 case_mask= 0;
  for (size_t curr_ind= 0; curr_ind < curr_len; ++curr_ind)
  {
    const uint16 *from= my_char_weight_addr_900(src, *curr);
    int page= *curr >> 8;
    int code= *curr & 0xFF;
    curr++;
    int ce_cnt= src->weights[page] ? UCA900_NUM_OF_CE(src->weights[page], code) : 0;
    changed_ce= 0;
    for (int i_ce= 0; i_ce < ce_cnt; ++i_ce)
    {
      uint16 primary_weight= from[i_ce * UCA900_DISTANCE_BETWEEN_WEIGHTS];
      if (primary_weight)
      {
        uint16 case_weight= from[i_ce * UCA900_DISTANCE_BETWEEN_WEIGHTS +
          2 * UCA900_DISTANCE_BETWEEN_LEVELS];
        uint16 *ce_to= 0;
        if (is_tertiary_weight_upper_case(case_weight))
        {
          if (!case_to_copy)
            upper_cnt++;
          else
            case_mask= CASE_FIRST_UPPER_MASK;
        }
        else
        {
          if (!case_to_copy)
            lower_cnt++;
          else
            case_mask= CASE_FIRST_LOWER_MASK;
        }
        if (case_to_copy)
        {
          do
          {
            ce_to= to + changed_ce * MY_UCA_900_CE_SIZE * to_stride;
            changed_ce++;
          } while (*ce_to == 0);
          ce_to[2 * to_stride]|= case_mask;
          case_to_copy--;
        }
      }
    }
  }
  if (origin_pri_cnt <= tailored_pri_cnt)
  {
    for (int i= origin_pri_cnt; i < tailored_pri_cnt; ++i)
    {
      if (to[changed_ce * MY_UCA_900_CE_SIZE * to_stride] &&
          to[changed_ce * MY_UCA_900_CE_SIZE * to_stride] < EXTRA_CE_PRI_BASE)
        to[(changed_ce * MY_UCA_900_CE_SIZE + 2) * to_stride]= 0;
    }
  }
  else
  {
    if (upper_cnt && lower_cnt)
      case_mask= CASE_FIRST_MIXED_MASK;
    else if (upper_cnt && !lower_cnt)
      case_mask= CASE_FIRST_UPPER_MASK;
    else
      case_mask= CASE_FIRST_LOWER_MASK;
    bool skipped_extra_ce= false;
    for (int i= tailored_ce_cnt - 1; i >= 0; --i)
    {
      if (to[i * MY_UCA_900_CE_SIZE * to_stride] &&
          to[i * MY_UCA_900_CE_SIZE * to_stride] < EXTRA_CE_PRI_BASE)
      {
        if ((r->before_level == 1 || r->diff[0]) && !skipped_extra_ce)
        {
          skipped_extra_ce= true;
          continue;
        }
        to[(i * MY_UCA_900_CE_SIZE + 2) * to_stride]|= case_mask;
        break;
      }
    }
  }
  spread_case_mask(to, to_stride, tailored_ce_cnt, case_mask);
}

static size_t
my_char_weight_put_900(MY_UCA_WEIGHT_LEVEL *dst, uint16 *to, size_t to_stride,
                       size_t to_length, uint16 *to_num_ce,
                       const MY_COLL_RULE *rule, size_t base_len, size_t curr_len)
{
  size_t count;
  int total_ce_cnt= 0;

  const my_wc_t *base= rule->base;
  for (count= 0; base_len; )
  {
    const uint16 *from= nullptr;
    size_t from_stride= 0;
    int ce_cnt= 0;

    for (size_t chlen= base_len; chlen > 1; chlen--)
    {
      if ((from= my_uca_contraction_weight(&dst->contractions, base, chlen)))
      {
        from_stride= 1;
        base+= chlen;
        base_len-= chlen;
        ce_cnt= *(from + MY_UCA_MAX_WEIGHT_SIZE - 1);
        break;
      }
    }

    if (!from)
    {
      int page= *base >> 8;
      int code= *base & 0xFF;
      base++;
      base_len--;
      if (dst->weights[page])
      {
        from= UCA900_WEIGHT_ADDR(
          dst->weights[page], /*level=*/0, code);
        from_stride= UCA900_DISTANCE_BETWEEN_LEVELS;
        ce_cnt= UCA900_NUM_OF_CE(dst->weights[page], code);
      }
    }

    for (int weight_ind= 0 ;
         weight_ind < ce_cnt * MY_UCA_900_CE_SIZE && count < to_length;
         weight_ind++)
    {
      *to= *from;
      to+= to_stride;
      from+= from_stride;
      count++;
    }
    total_ce_cnt+= ce_cnt;
  }

  /*
    For shift on primary weight, there might be no enough room in the tables.
    For example, Sihala has the rule "&\\u0DA5 < \\u0DA4", which means
    that we should move U+0DA4 after U+0DA5 (on the primary level).
    However, there is no room after U+0DA5 in DUCET unless we wanted to
    conflict with U+0DA6:

      0DA4  ; [.28EC.0020.0002] # SINHALA LETTER TAALUJA NAASIKYAYA
      0DA5  ; [.28ED.0020.0002] # SINHALA LETTER TAALUJA SANYOOGA NAAKSIKYAYA
      0DA6  ; [.28EE.0020.0002] # SINHALA LETTER SANYAKA JAYANNA

    Before our implementation of UCA 9.0.0, the shift on primary weight was
    done by making it a fake expansion when parsing the rule, where we'd expand
    U+0DA4 to U+0DA5 U+MMMM, MMMM being 'last_non_ignorable'. (This happens
    in my_coll_parser_scan_reset_sequence()). But from UCA 9.0.0, we also
    support accent- and case-sensitive collations, and then, having the extra
    weights of 'last_non_ignorable' (which is just a random character) on the
    second and third level may cause unexpected results for algorithms that
    use the meaning of the tertiary weight to infer case. Thus, we'll abandon
    the fake expansion way; instead, instead add an extra CE (after the one
    from U+0DA5, the character we are moving after) to represent all the
    weights we might want to shift. The actual shifting happens in
    apply_shift_900().

    For the rule "&\\u0DA5 < \\u0DA4", U+0DA4's weights become
    [.28ED.0020.0002][.54A4.0000.0000], where 0x54A4 is the value of
    EXTRA_CE_PRI_BASE. We then apply the differences from the rule
    (which are never negative) to the last CE, so that it becomes
    e.g. [.54A5.0000.0000].
  */
  if ((rule->diff[0] || rule->diff[1] || rule->diff[2]) && count < to_length)
  {
    *to= rule->diff[0] ? EXTRA_CE_PRI_BASE: 0;
    to+= to_stride;
    *to= rule->diff[1] ? EXTRA_CE_SEC_BASE: 0;
    to+= to_stride;
    *to= rule->diff[2] ? EXTRA_CE_TER_BASE: 0;
    to+= to_stride;
    total_ce_cnt++;
    count+= 3;
  }
  total_ce_cnt= std::min(total_ce_cnt,
                         (MY_UCA_MAX_WEIGHT_SIZE - 1) / MY_UCA_900_CE_SIZE);
  *to_num_ce= total_ce_cnt;

  return total_ce_cnt;
}

/**
  Helper function:
  Copies UCA weights for a given "uint" string
  to the given location.
  
  @param cs         character set
  @param dst        destination UCA weight data
  @param to         destination address
  @param to_stride  number of bytes between each successive weight in "to"
  @param to_length  size of destination
  @param to_num_ce  where to put the number of CEs generated
  @param rule       The rule that contains the characters whose weight
                    are to copied
  @param base_len   The length of base character list
  @param curr_len   The length of shift character list
  @param uca_ver    UCA version
  
  @return    number of weights put
*/

static size_t
my_char_weight_put(const CHARSET_INFO *cs, MY_UCA_WEIGHT_LEVEL *dst, uint16 *to,
                   size_t to_stride, size_t to_length, uint16 *to_num_ce,
                   const MY_COLL_RULE *rule, size_t base_len, size_t curr_len, enum_uca_ver uca_ver)
{
  if (uca_ver == UCA_V900)
    return my_char_weight_put_900(dst, to, to_stride, to_length, to_num_ce,
                                  rule, base_len, curr_len);

  const my_wc_t *base= rule->base;
  size_t count= 0;
  while (base_len != 0)
  {
    const uint16 *from= NULL;

    for (size_t chlen= base_len; chlen > 1; chlen--)
    {
      if ((from= my_uca_contraction_weight(&dst->contractions, base, chlen)))
      {
        base+= chlen;
        base_len-= chlen;
        break;
      }
    }

    if (!from)
    {
      from= my_char_weight_addr(dst, *base);
      base++;
      base_len--;
    }

    for ( ; from && *from && count < to_length; )
    {
      *to= *from++;
      to+= to_stride;
      count++;
    }
  }

  *to= 0;
  return count;
}


/**
  Alloc new page and copy the default UCA weights
  @param cs       Character set
  @param loader   Character set loader
  @param src      Default UCA data to copy from
  @param dst      UCA data to copy weights to
  @param page     page number

  @return
  @retval         FALSE on success
  @retval         TRUE  on error
*/
static my_bool
my_uca_copy_page(CHARSET_INFO *cs,
                 MY_CHARSET_LOADER *loader,
                 const MY_UCA_WEIGHT_LEVEL *src,
                 MY_UCA_WEIGHT_LEVEL *dst,
                 size_t page)
{
  const uint dst_size= 256 * dst->lengths[page] * sizeof(uint16);
  if (!(dst->weights[page]= (uint16 *) (loader->once_alloc)(dst_size)))
    return TRUE;

  DBUG_ASSERT(src->lengths[page] <= dst->lengths[page]);
  memset(dst->weights[page], 0, dst_size);
  if (cs->uca && cs->uca->version == UCA_V900)
  {
    const uint src_size= 256 * src->lengths[page] * sizeof(uint16);
    memcpy(dst->weights[page], src->weights[page], src_size);
  }
  else if (src->lengths[page] > 0)
  {
    for (uint chc=0 ; chc < 256; chc++)
    {
      memcpy(dst->weights[page] + chc * dst->lengths[page],
             src->weights[page] + chc * src->lengths[page],
             src->lengths[page] * sizeof(uint16));
    }
  }
  return FALSE;
}

static bool
apply_shift_900(CHARSET_INFO *cs, MY_CHARSET_LOADER *loader,
                MY_COLL_RULES *rules, MY_COLL_RULE *r, int level,
                uint16 *to, size_t to_stride, size_t nweights)
{
  /* Apply level difference. */
  if (nweights)
  {
    uint16 * const last_weight_ptr = to + (nweights - 1) * to_stride * MY_UCA_900_CE_SIZE;
    last_weight_ptr[0]+= r->diff[0];
    last_weight_ptr[to_stride]+= r->diff[1];
    last_weight_ptr[to_stride * 2]+= r->diff[2];
    if (r->before_level == 1) /* Apply "&[before primary]" */
    {
      int last_sec_pri_pos= 0;
      for (int i= nweights - 2; i >= 0; --i)
      {
        if (to[i * to_stride * MY_UCA_900_CE_SIZE])
        {
          last_sec_pri_pos= i;
          break;
        }
      }
      if (last_sec_pri_pos >= 0)
      {
        to[last_sec_pri_pos * to_stride * MY_UCA_900_CE_SIZE]--; /* Reset before */
        if (rules->shift_after_method == my_shift_method_expand)
        {
          /*
            Special case. Don't let characters shifted after X
            and before next(X) intermix to each other.

            For example:
            "[shift-after-method expand] &0 < a &[before primary]1 < A".
            I.e. we reorder 'a' after '0', and then 'A' before '1'.
            'a' must be sorted before 'A'.

            Note, there are no real collations in CLDR which shift
            after and before two neighbourgh characters. We need this
            just in case. Reserving 4096 (0x1000) weights for such
            cases is perfectly enough.
          */
          /* W3-TODO: const may vary on levels 2,3*/
          last_weight_ptr[0]+= 0x1000;
        }
      }
      else
      {
        my_snprintf(loader->error, sizeof(loader->error),
                    "Can't reset before "
                    "a primary ignorable character U+%04lX", r->base[0]);
        return TRUE;
      }
    }
  }
  else
  {
    /* Shift to an ignorable character, e.g.: & \u0000 < \u0001 */
    DBUG_ASSERT(to[0] == 0);
    to[0]= r->diff[level];
  }
  return FALSE;
}

static my_bool
apply_shift(CHARSET_INFO *cs, MY_CHARSET_LOADER *loader,
            MY_COLL_RULES *rules, MY_COLL_RULE *r, int level,
            uint16 *to, size_t to_stride, size_t nweights)
{
  if (rules->uca->version == UCA_V900)
    return apply_shift_900(cs, loader, rules, r, level, to, to_stride, nweights);

  DBUG_ASSERT(to_stride == 1);

  /* Apply level difference. */
  if (nweights)
  {
    to[nweights - 1]+= r->diff[0];
    if (r->before_level == 1) /* Apply "&[before primary]" */
    {
      if (nweights >= 2)
      {
        to[nweights - 2]--; /* Reset before */
        if (rules->shift_after_method == my_shift_method_expand)
        {
          /*
            Special case. Don't let characters shifted after X
            and before next(X) intermix to each other.
            
            For example:
            "[shift-after-method expand] &0 < a &[before primary]1 < A".
            I.e. we reorder 'a' after '0', and then 'A' before '1'.
            'a' must be sorted before 'A'.
            
            Note, there are no real collations in CLDR which shift
            after and before two neighbourgh characters. We need this
            just in case. Reserving 4096 (0x1000) weights for such
            cases is perfectly enough.
          */
          /* W3-TODO: const may vary on levels 2,3*/
          to[nweights - 1]+= 0x1000;
        }
      }
      else
      {
        my_snprintf(loader->error, sizeof(loader->error),
                    "Can't reset before "
                    "a primary ignorable character U+%04lX", r->base[0]);
        return TRUE;
      }
    }
  }
  else
  {
    /* Shift to an ignorable character, e.g.: & \u0000 < \u0001 */
    DBUG_ASSERT(to[0] == 0);
    to[0]= r->diff[level];
  }
  return FALSE; 
}


static my_bool
apply_one_rule(CHARSET_INFO *cs, MY_CHARSET_LOADER *loader,
               MY_COLL_RULES *rules, MY_COLL_RULE *r, int level,
               MY_UCA_WEIGHT_LEVEL *dst)
{
  size_t nweights;
  size_t nreset= my_coll_rule_reset_length(r); /* Length of reset sequence */
  size_t nshift= my_coll_rule_shift_length(r); /* Length of shift sequence */
  uint16 *to, *to_num_ce;
  size_t to_stride;

  if (nshift >= 2) /* Contraction */
  {
    size_t i;
    int flag;
    MY_CONTRACTIONS *contractions= &dst->contractions;
    /* Add HEAD, MID and TAIL flags for the contraction parts */
    my_uca_add_contraction_flag(contractions, r->curr[0],
                                r->with_context ?
                                MY_UCA_PREVIOUS_CONTEXT_HEAD :
                                MY_UCA_CNT_HEAD);
    for (i= 1, flag= MY_UCA_CNT_MID1; i < nshift - 1; i++, flag<<= 1)
      my_uca_add_contraction_flag(contractions, r->curr[i], flag);
    my_uca_add_contraction_flag(contractions, r->curr[i],
                                r->with_context ?
                                MY_UCA_PREVIOUS_CONTEXT_TAIL :
                                MY_UCA_CNT_TAIL);
    /* Add new contraction to the contraction list */
    to= my_uca_add_contraction(contractions, r->curr, nshift,
                               r->with_context)->weight;
    to_stride= 1;
    to_num_ce= &to[MY_UCA_MAX_WEIGHT_SIZE - 1];
    /* Store weights of the "reset to" character */
    dst->contractions.nitems--; /* Temporarily hide - it's incomplete */
    nweights= my_char_weight_put(cs, dst, to, to_stride, MY_UCA_MAX_WEIGHT_SIZE - 1,
                                 to_num_ce, r, nreset, nshift, rules->uca->version);
    dst->contractions.nitems++; /* Activate, now it's complete */
  }
  else
  {
    my_wc_t pagec= (r->curr[0] >> 8);
    DBUG_ASSERT(dst->weights[pagec]);
    if (cs->uca && cs->uca->version == UCA_V900)
    {
      to= my_char_weight_addr_900(dst, r->curr[0]);
      to_stride= UCA900_DISTANCE_BETWEEN_LEVELS;
      to_num_ce= to - UCA900_DISTANCE_BETWEEN_LEVELS;
    }
    else
    {
      to= my_char_weight_addr(dst, r->curr[0]);
      to_stride= 1;
      to_num_ce= to + (dst->lengths[pagec] - 1);
    }
    /* Store weights of the "reset to" character */
    if (dst->lengths[pagec] == 0)
      nweights= 0;
    else
      nweights= my_char_weight_put(cs, dst, to, to_stride, dst->lengths[pagec] - 1,
                                   to_num_ce, r, nreset, nshift, rules->uca->version);
  }

  change_weight_if_case_first(cs, r, to, to_stride, nshift, nweights);
  /* Apply level difference. */
  return apply_shift(cs, loader, rules, r, level, to, to_stride, nweights);
}


/**
  Check if collation rules are valid,
  i.e. characters are not outside of the collation suported range.
*/
static int
check_rules(MY_CHARSET_LOADER *loader,
            const MY_COLL_RULES *rules,
            const MY_UCA_WEIGHT_LEVEL *dst, const MY_UCA_WEIGHT_LEVEL *src)
{
  const MY_COLL_RULE *r, *rlast;
  for (r= rules->rule, rlast= rules->rule + rules->nrules; r < rlast; r++)
  {
    if (r->curr[0] > dst->maxchar)
    {
      my_snprintf(loader->error, sizeof(loader->error),
                  "Shift character out of range: u%04X", (uint) r->curr[0]);
      return TRUE;
    }
    else if (r->base[0] > src->maxchar)
    {
      my_snprintf(loader->error, sizeof(loader->error),
                  "Reset character out of range: u%04X", (uint) r->base[0]);
      return TRUE;
    }
  }
  return FALSE;
}

static void
synthesize_lengths_900(uchar *lengths,
                       const uint16 * const * weights, uint npages)
{
  for (uint page= 0; page < npages; ++page)
  {
    int max_len= 0;
    if (weights[page])
    {
      for (uint code= 0; code < 256; ++code)
      {
        max_len= std::max<int>(max_len, weights[page][code]);
      }
    }
    if (max_len == 0)
      lengths[page]= 0;
    else
      lengths[page]= max_len * MY_UCA_900_CE_SIZE + 1;
  }
}

static my_bool
init_weight_level(CHARSET_INFO *cs, MY_CHARSET_LOADER *loader,
                  MY_COLL_RULES *rules, int level,
                  MY_UCA_WEIGHT_LEVEL *dst, const MY_UCA_WEIGHT_LEVEL *src,
                  bool lengths_are_temporary)
{
  MY_COLL_RULE *r, *rlast;
  int ncontractions= 0;
  size_t i, npages= (src->maxchar + 1) / 256;

  dst->maxchar= src->maxchar;

  if (check_rules(loader, rules, dst, src))
    return TRUE;

  /* Allocate memory for pages and their lengths */
  if (lengths_are_temporary)
  {
    if (!(dst->lengths= (uchar *) (loader->mem_malloc)(npages)))
      return TRUE;
    if (!(dst->weights= (uint16 **) (loader->once_alloc)(npages *
                                                         sizeof(uint16 *))))
    {
      (loader->mem_free)(dst->lengths);
      return TRUE;
    }
  }
  else
  {
    if (!(dst->lengths= (uchar *) (loader->once_alloc)(npages)) ||
        !(dst->weights= (uint16 **) (loader->once_alloc)(npages *
                                                         sizeof(uint16 *))))
      return TRUE;
  }

  /*
    Copy pages lengths and page pointers from the default UCA weights.
  */
  memcpy(dst->lengths, src->lengths, npages);
  memcpy(dst->weights, src->weights, npages * sizeof(uint16 *));

  /*
    Calculate maximum lenghts for the pages which will be overwritten.
    Mark pages that will be overwritten as NULL.
    We'll allocate their own memory.
  */
  for (r= rules->rule, rlast= rules->rule + rules->nrules; r < rlast; r++)
  {
    if (!r->curr[1]) /* If not a contraction */
    {
      uint pagec= (r->curr[0] >> 8);
      if (r->base[1]) /* Expansion */
      {
        /* Reserve space for maximum possible length */
        dst->lengths[pagec]= MY_UCA_MAX_WEIGHT_SIZE;
      }
      else
      {
        uint pageb= (r->base[0] >> 8);
        if ((r->diff[0] || r->diff[1] || r->diff[2]) &&
            dst->lengths[pagec] < (src->lengths[pageb] + 3))
        {
          if ((src->lengths[pageb] + 3) > MY_UCA_MAX_WEIGHT_SIZE)
            dst->lengths[pagec]= MY_UCA_MAX_WEIGHT_SIZE;
          else
            dst->lengths[pagec]= src->lengths[pageb] + 3;
        }
        else if (dst->lengths[pagec] < src->lengths[pageb])
          dst->lengths[pagec]= src->lengths[pageb];
      }
      dst->weights[pagec]= NULL; /* Mark that we'll overwrite this page */
    }
    else
      ncontractions++;
  }

  /* Allocate pages that we'll overwrite and copy default weights */
  for (i= 0; i < npages; i++)
  {
    my_bool rc;
    /*
      Don't touch pages with lengths[i]==0, they have implicit weights
      calculated algorithmically.
    */
    if (!dst->weights[i] && dst->lengths[i] &&
        (rc= my_uca_copy_page(cs, loader, src, dst, i)))
      return rc;
  }

  if (ncontractions)
  {
    if (my_uca_alloc_contractions(&dst->contractions, loader, ncontractions))
      return TRUE;
  }

  /*
    Preparatory step is done at this point.
    Now we have memory allocated for the pages that we'll overwrite,
    and for contractions, including previous context contractions.
    Also, for the pages that we'll overwrite, we have copied default weights.
    Now iterate through the rules, overwrite weights for the characters
    that appear in the rules, and put all contractions into contraction list.
  */
  for (r= rules->rule; r < rlast;  r++)
  {
    if (apply_one_rule(cs, loader, rules, r, level, dst))
      return TRUE;
  }
  return FALSE;
}

/**
  Check whether the composition character is already in rule list
  @param  rules   The rule list
  @param  wc      The composition character
  @return true    The composition character is already in list
          false   The composition character is not in list
*/
static bool
my_comp_in_rulelist(const MY_COLL_RULES *rules, my_wc_t wc)
{
  MY_COLL_RULE *r, *rlast;
  for (r= rules->rule, rlast= rules->rule + rules->nrules; r < rlast; r++)
  {
    if (r->curr[0] == wc && r->curr[1] == 0)
      return true;
  }
  return false;
}

/**
  Check whether a composition character in the decomposition list is a
  normal character.
  @param  dec_ind   The index of composition character in list
  @return           Whether it is a normal character
*/
static inline bool my_compchar_is_normal_char(uint dec_ind)
{
  return uni_dec[dec_ind].decomp_tag == DECOMP_TAG_NONE;
}

static inline bool my_compchar_is_normal_char(const Unidata_decomp *decomp)
{
  return my_compchar_is_normal_char(decomp - std::begin(uni_dec));
}

static void get_decomposition(my_wc_t *origin_dec)
{
  auto comp_func= [](Unidata_decomp x, Unidata_decomp y){
    return x.charcode < y.charcode;
  };
  Unidata_decomp to_find= {
    origin_dec[0], CHAR_CATEGORY_LU, DECOMP_TAG_NONE, {0}
  };
  Unidata_decomp *decomp= std::lower_bound(std::begin(uni_dec),
                                           std::end(uni_dec),
                                           to_find,
                                           comp_func);
  if (decomp == std::end(uni_dec) || decomp->charcode != origin_dec[0])
    return;
  memcpy(origin_dec, decomp->dec_codes,
         MY_UCA_MAX_EXPANSION * sizeof(origin_dec[0]));
  return;
}

static Combining_mark* my_find_combining_mark(my_wc_t code)
{
  auto comp_func= [](Combining_mark x, Combining_mark y){
    return x.charcode < y.charcode;
  };
  Combining_mark to_find= {code, 0};
  return std::lower_bound(std::begin(combining_marks),
                          std::end(combining_marks),
                          to_find, comp_func);
}

/**
  Check if a list of combining marks contains the whole list of origin
  decomposed combining marks.
  @param    origin_dec    The origin list of combining marks decomposed from
                          character in tailoring rule.
  @param    dec_codes     The list of combining marks decomposed from
                          character in decomposition list.
  @param    dec_diff      The combining marks exist in dec_codes but not in
                          origin_dec.
  @return                 Whether the list of combining marks contains the
                          whole list of origin combining marks.
*/
static bool
my_is_inheritance_of_origin(const my_wc_t *origin_dec,
                            const my_wc_t *dec_codes,
                            my_wc_t *dec_diff)
{
  int ind0, ind1, ind2;
  if (origin_dec[0] != dec_codes[0])
    return false;
  for (ind0= ind1= ind2= 1;
       ind0 < MY_UCA_MAX_CONTRACTION && ind1 < MY_UCA_MAX_CONTRACTION &&
       origin_dec[ind0] && dec_codes[ind1];)
  {
    if (origin_dec[ind0] == dec_codes[ind1])
    {
      ind0++;
      ind1++;
    }
    else
    {
      Combining_mark *mark0= my_find_combining_mark(origin_dec[ind0]);
      Combining_mark *mark1= my_find_combining_mark(dec_codes[ind1]);
      if (mark0->ccc == mark1->ccc)
        return false;
      dec_diff[ind2++]= dec_codes[ind1++];
    }
  }
  if (ind0 >= MY_UCA_MAX_CONTRACTION || !origin_dec[ind0])
  {
    while (ind1 < MY_UCA_MAX_CONTRACTION)
    {
      dec_diff[ind2++]= dec_codes[ind1++];
    }
    return true;
  }
  return false;
}

/**
  Add new rules recersively if one rule's characters are in decomposition
  list.
  @param          rules       The rule list
  @param          r           The rule to check
  @param          origin_dec  The origin list of combining marks decomposed
                              from character in tailoring rule.
  @param          comp_added  Bitset which marks whether the comp
                              character has been added to rule list.
  @return 1       Error adding new rules
          0       Add rules successfully
*/
static int
my_coll_add_inherit_rules(MY_COLL_RULES *rules,
                          MY_COLL_RULE *r,
                          const my_wc_t *origin_dec,
                          std::bitset<array_elements(uni_dec)> *comp_added)
{
  for (uint dec_ind= 0; dec_ind < array_elements(uni_dec); dec_ind++)
  {
    /*
      For normal character which can be decomposed, it is always
      decomposed to be another character and one combining mark.
    */
    if (!my_compchar_is_normal_char(dec_ind) || comp_added->test(dec_ind))
      continue;
    /*
      In DUCET, all accented character's weight is defined as base
      character's weight followed by accent mark's weight. For example:
      00DC = 0055 + 0308
      0055  ; [.1E30.0020.0008]                  # LATIN CAPITAL LETTER U
      0308  ; [.0000.002B.0002]                  # COMBINING DIAERESIS
      00DC  ; [.1E30.0020.0008][.0000.002B.0002] # LATIN CAPITAL LETTER U
                                                   WITH DIAERESIS
      So the composition character's rule should be same as origin rule
      except of the change of curr value.
    */
    my_wc_t dec_diff[MY_UCA_MAX_CONTRACTION]{r->curr[0], 0};
    if (my_is_inheritance_of_origin(origin_dec, uni_dec[dec_ind].dec_codes,
                                    dec_diff) &&
        !my_comp_in_rulelist(rules, uni_dec[dec_ind].charcode))
    {
      MY_COLL_RULE newrule{{0}, {uni_dec[dec_ind].charcode, 0}, {0}, 0, false};
      memcpy(newrule.base, dec_diff, sizeof(newrule.base));
      if (my_coll_rules_add(rules, &newrule))
        return 1;
      comp_added->set(dec_ind);
    }
  }
  return 0;
}

static bool
combining_mark_in_rulelist(const my_wc_t *dec_codes,
                           const MY_COLL_RULE *r_start,
                           const MY_COLL_RULE *r_end)
{
  for (int i= 1; i < MY_UCA_MAX_CONTRACTION; ++i)
  {
    if (!*(dec_codes + i))
      return false;
    for (const MY_COLL_RULE *r= r_start; r < r_end; ++r)
    {
      if (r->curr[0] == *(dec_codes + i))
      {
        return true;
      }
    }
  }
  return false;
}

static int
add_normalization_rules(const CHARSET_INFO *cs, MY_COLL_RULES *rules)
{
  if (!cs->coll_param || !cs->coll_param->norm_enabled)
    return 0;
  const int orig_rule_num= rules->nrules;
  for (Unidata_decomp *decomp= std::begin(uni_dec);
       decomp < std::end(uni_dec); ++decomp)
  {
    if (!my_compchar_is_normal_char(decomp) ||
        my_comp_in_rulelist(rules, decomp->charcode) ||
        !combining_mark_in_rulelist(decomp->dec_codes, rules->rule,
                                    rules->rule + orig_rule_num))
      continue;
    MY_COLL_RULE newrule{{0}, {decomp->charcode, 0},{0}, 0, false};
    memcpy(newrule.base, decomp->dec_codes, sizeof(newrule.base));
    if (my_coll_rules_add(rules, &newrule))
      return 1;
  }
  return 0;
}

/**
  For every rule in rule list, check and add new rules if it is in
  decomposition list.
  @param  rules The rule list
  @return 1     Error happens when adding new rule
          0     Add rules successfully
*/
static int
my_coll_check_rule_and_inherit(MY_COLL_RULES *rules)
{
  if (rules->uca->version != UCA_V900)
    return 0;

  /*
    Character can combine with marks to be a new character. For example,
    A + [mark b] = A1, A1 + [mark c] = A2. We think the weight of A1 and
    A2 should shift with A if A is in rule list and its weight shifts,
    unless A1 / A2 is already in rule list.
  */
  std::bitset<array_elements(uni_dec)> comp_added;
  int orig_rule_num= rules->nrules;
  for (int i= 0; i < orig_rule_num; ++i)
  {
    MY_COLL_RULE r= *(rules->rule + i);
    /* Do not add inheritance rule for contraction */
    if (r.curr[1])
      continue;
    my_wc_t origin_dec[MY_UCA_MAX_CONTRACTION]= {r.curr[0], 0};
    get_decomposition(origin_dec);
    if (my_coll_add_inherit_rules(rules, &r, origin_dec, &comp_added))
      return 1;
  }
  return 0;
}

/**
  Helper function to store weight boundary values.
  @param[out] wt_rec     Weight boundary for each character group and gap
                         between groups
  @param      rec_ind    The position from where to store weight boundary
  @param      old_begin  Beginning weight of character group before reorder
  @param      old_end    End weight of character group before reorder
  @param      new_begin  Beginning weight of character group after reorder
  @param      new_end    End weight of character group after reorder
*/
static inline void
my_set_weight_rec(Reorder_wt_rec(&wt_rec)[2 * UCA_MAX_CHAR_GRP],
                  int rec_ind, uint16 old_begin, uint16 old_end,
                  uint16 new_begin, uint16 new_end)
{
  wt_rec[rec_ind] = {{old_begin, old_end}, {new_begin, new_end}};
}

/**
  Calculate the reorder parameters for the character groups.
  @param      cs         Character set info
  @param[out] rec_ind    The position from where to store weight boundary
*/
static void
my_calc_char_grp_param(const CHARSET_INFO *cs, int &rec_ind)
{
  int weight_start= START_WEIGHT_TO_REORDER;
  int grp_ind= 0;
  Reorder_param *param= cs->coll_param->reorder_param;
  for (; grp_ind < UCA_MAX_CHAR_GRP; ++grp_ind)
  {
    if (param->reorder_grp[grp_ind] == CHARGRP_NONE)
      break;
    for (Char_grp_info *info= std::begin(char_grp_infos);
         info < std::end(char_grp_infos); ++info)
    {
      if (param->reorder_grp[grp_ind] != info->group)
        continue;
      my_set_weight_rec(param->wt_rec, grp_ind, info->grp_wt_bdy.begin,
                        info->grp_wt_bdy.end, weight_start,
                        weight_start + info->grp_wt_bdy.end -
                        info->grp_wt_bdy.begin);
      weight_start= param->wt_rec[grp_ind].new_wt_bdy.end + 1;
      break;
    }
  }
  rec_ind= grp_ind;
}

/**
  Calculate the reorder parameters for the gap between character groups.
  @param      cs         Character set info
  @param      rec_ind    The position from where to store weight boundary
*/
static void
my_calc_char_grp_gap_param(CHARSET_INFO *cs, int rec_ind)
{
  Reorder_param *param= cs->coll_param->reorder_param;
  uint16 weight_start= param->wt_rec[rec_ind - 1].new_wt_bdy.end + 1;
  Char_grp_info *last_grp= NULL;
  for (Char_grp_info *info= std::begin(char_grp_infos);
       info < std::end(char_grp_infos); ++info)
  {
    for (int ind= 0; ind < UCA_MAX_CHAR_GRP; ++ind)
    {
      if (param->reorder_grp[ind] == CHARGRP_NONE)
        break;
      if (param->reorder_grp[ind] != info->group)
        continue;
      if (param->max_weight < info->grp_wt_bdy.end)
        param->max_weight= info->grp_wt_bdy.end;
      /*
        There might be some character groups before the first character
        group in our list.
      */
      if (!last_grp && info->grp_wt_bdy.begin > START_WEIGHT_TO_REORDER)
      {
        my_set_weight_rec(param->wt_rec, rec_ind, START_WEIGHT_TO_REORDER,
                          info->grp_wt_bdy.begin - 1, weight_start,
                          weight_start +
                          (info->grp_wt_bdy.begin - 1) -
                          START_WEIGHT_TO_REORDER);
        weight_start= param->wt_rec[rec_ind].new_wt_bdy.end + 1;
        rec_ind++;
      }
      /* Gap between 2 character groups in out list. */
      if (last_grp && last_grp->grp_wt_bdy.end < (info->grp_wt_bdy.begin - 1))
      {
        my_set_weight_rec(param->wt_rec, rec_ind, last_grp->grp_wt_bdy.end + 1,
                          info->grp_wt_bdy.begin - 1, weight_start,
                          weight_start +
                          (info->grp_wt_bdy.begin - 1) -
                          (last_grp->grp_wt_bdy.end + 1));
        weight_start= param->wt_rec[rec_ind].new_wt_bdy.end + 1;
        rec_ind++;
      }
      last_grp= info;
      break;
    }
  }
}



/**
  Prepare reorder parameters.
  @param  cs     Character set info
*/
static void my_prepare_reorder(CHARSET_INFO *cs)
{
  if (!cs->coll_param->reorder_param)
    return;
  /*
    For each group of character, for example, latin characters,
    their weights are in a seperate range. The default sequence
    of these groups is: Latin, Greek, Coptic, Cyrillic, and so
    on. Some languages want to change the default sequence. For
    example, Croatian wants to put Cyrillic to just behind Latin.
    We need to reorder the character groups and change their
    weight accordingly. Here we calculate the parameters needed
    for weight change. And the change will happen when weight
    returns from strnxfrm.
  */
  int rec_ind= 0;
  my_calc_char_grp_param(cs, rec_ind);
  my_calc_char_grp_gap_param(cs, rec_ind);
}

/**
  Prepare parametric tailoring, like reorder, etc.
  @param  cs     Character set info
  @param  rules  Collation rule list to add to.
  @return false  Collation parameters applied sucessfully.
          true   Error happened.
*/
static bool my_prepare_coll_param(CHARSET_INFO *cs, MY_COLL_RULES *rules)
{
  if (rules->uca->version != UCA_V900 || !cs->coll_param)
    return false;

  my_prepare_reorder(cs);
  if (add_normalization_rules(cs, rules))
    return true;
  /* Might add other parametric tailoring rules later. */
  return false;
}
/*
  This function copies an UCS2 collation from
  the default Unicode Collation Algorithm (UCA)
  weights applying tailorings, i.e. a set of
  alternative weights for some characters. 
  
  The default UCA weights are stored in uca_weight/uca_length.
  They consist of 256 pages, 256 character each.
  
  If a page is not overwritten by tailoring rules,
  it is copies as is from UCA as is.
  
  If a page contains some overwritten characters, it is
  allocated. Untouched characters are copied from the
  default weights.
*/

static my_bool
create_tailoring(CHARSET_INFO *cs, MY_CHARSET_LOADER *loader)
{
  MY_COLL_RULES rules;
  MY_UCA_INFO new_uca, *src_uca= NULL;
  int rc= 0;
  MY_UCA_WEIGHT_LEVEL *src, *dst;
  size_t npages;
  bool lengths_are_temporary;

  *loader->error= '\0';

  if (!cs->tailoring)
    return 0; /* Ok to add a collation without tailoring */

  memset(&rules, 0, sizeof(rules));
  rules.loader= loader;
  rules.uca= cs->uca ? cs->uca : &my_uca_v400; /* For logical positions, etc */
  memset(&new_uca, 0, sizeof(new_uca));

  /* Parse ICU Collation Customization expression */
  if ((rc= my_coll_rule_parse(&rules,
                              cs->tailoring,
                              cs->tailoring + strlen(cs->tailoring),
                              cs->name)))
    goto ex;

  if ((rc= my_coll_check_rule_and_inherit(&rules)))
    goto ex;

  if ((rc= my_prepare_coll_param(cs, &rules)))
    goto ex;

  if (rules.uca->version == UCA_V520)       /* Unicode-5.2.0 requested */
  {
    src_uca= &my_uca_v520;
    cs->caseinfo= &my_unicase_unicode520;
  }
  else if (rules.uca->version == UCA_V400)  /* Unicode-4.0.0 requested */
  {
    src_uca= &my_uca_v400;
    if (!cs->caseinfo)
      cs->caseinfo= &my_unicase_default;
  }
  else                                      /* No Unicode version specified */
  {
    src_uca= cs->uca ? cs->uca : &my_uca_v400;
    if (!cs->caseinfo)
      cs->caseinfo= &my_unicase_default;
  }

  /*
    For UCA 9.0.0, we don't have a length page, but we still create one
    temporarily so that we can keep track of much memory we need to
    allocate for weights.
  */
  src= &src_uca->level[0];
  dst= &new_uca.level[0];
  npages= (src->maxchar + 1) / 256;
  if (rules.uca->version == UCA_V900)
  {
    if (!(src->lengths= (uchar *) (loader->mem_malloc)(npages)))
      goto ex;
    synthesize_lengths_900(src->lengths, src->weights, npages);
  }

  lengths_are_temporary= (rules.uca->version == UCA_V900);
  if ((rc= init_weight_level(cs, loader, &rules, 0, dst, src,
                             lengths_are_temporary)))
    goto ex;

  if (lengths_are_temporary)
  {
    (loader->mem_free)(src->lengths);
    (loader->mem_free)(dst->lengths);
    src->lengths= nullptr;
    dst->lengths= nullptr;
  }

  new_uca.version= src_uca->version;
  if (!(cs->uca= (MY_UCA_INFO *) (loader->once_alloc)(sizeof(MY_UCA_INFO))))
  {
    rc= 1;
    goto ex;
  }
  cs->uca[0]= new_uca;

ex:
  (loader->mem_free)(rules.rule);
  if (rc != 0 && loader->error[0])
    loader->reporter(ERROR_LEVEL, "%s", loader->error);
  return rc;
}


/*
  Universal CHARSET_INFO compatible wrappers
  for the above internal functions.
  Should work for any character set.
*/

extern "C" {
static my_bool
my_coll_init_uca(CHARSET_INFO *cs, MY_CHARSET_LOADER *loader)
{
  cs->pad_char= ' ';
  cs->ctype= my_charset_utf8_unicode_ci.ctype;
  if (!cs->caseinfo)
    cs->caseinfo= &my_unicase_default;
  return create_tailoring(cs, loader);
}

static int my_strnncoll_any_uca(const CHARSET_INFO *cs,
                                const uchar *s, size_t slen,
                                const uchar *t, size_t tlen,
                                my_bool t_is_prefix)
{
  if (cs->cset->mb_wc == my_mb_wc_utf8mb4_thunk)
  {
    return my_strnncoll_uca<uca_scanner_any<Mb_wc_utf8mb4>>(
      cs, Mb_wc_utf8mb4(), s, slen, t, tlen, t_is_prefix);
  }

  Mb_wc_through_function_pointer mb_wc(cs);
  return my_strnncoll_uca<uca_scanner_any<decltype(mb_wc)>>(
    cs, mb_wc, s, slen, t, tlen, t_is_prefix);
}

static int my_strnncollsp_any_uca(const CHARSET_INFO *cs,
                                  const uchar *s, size_t slen,
                                  const uchar *t, size_t tlen)
{
  if (cs->cset->mb_wc == my_mb_wc_utf8mb4_thunk)
  {
    return my_strnncollsp_uca(cs, Mb_wc_utf8mb4(), s, slen, t, tlen);
  }

  Mb_wc_through_function_pointer mb_wc(cs);
  return my_strnncollsp_uca(cs, mb_wc, s, slen, t, tlen);
}

static void my_hash_sort_any_uca(const CHARSET_INFO *cs,
                                 const uchar *s, size_t slen,
                                 ulong *n1, ulong *n2)
{
  if (cs->cset->mb_wc == my_mb_wc_utf8mb4_thunk) {
    my_hash_sort_uca(cs, Mb_wc_utf8mb4(), s, slen, n1, n2);
  } else {
    Mb_wc_through_function_pointer mb_wc(cs);
    my_hash_sort_uca(cs, mb_wc, s, slen, n1, n2);
  }
}

static size_t my_strnxfrm_any_uca(const CHARSET_INFO *cs, 
                                  uchar *dst, size_t dstlen, uint nweights,
                                  const uchar *src, size_t srclen, uint flags)
{
  if (cs->cset->mb_wc == my_mb_wc_utf8mb4_thunk)
  {
    return my_strnxfrm_uca(cs, Mb_wc_utf8mb4(), dst, dstlen, nweights,
                           src, srclen, flags);
  }

  Mb_wc_through_function_pointer mb_wc(cs);
  return my_strnxfrm_uca(cs, mb_wc, dst, dstlen, nweights,
                         src, srclen, flags);
}

static int my_strnncoll_uca_900(const CHARSET_INFO *cs,
                                const uchar *s, size_t slen,
                                const uchar *t, size_t tlen,
                                my_bool t_is_prefix)
{
  if (cs->cset->mb_wc == my_mb_wc_utf8mb4_thunk)
  {
    switch (cs->levels_for_compare)
    {
    case 1:
      return my_strnncoll_uca<uca_scanner_900<Mb_wc_utf8mb4, 1>>(
        cs, Mb_wc_utf8mb4(), s, slen, t, tlen, t_is_prefix);
    case 2:
      return my_strnncoll_uca<uca_scanner_900<Mb_wc_utf8mb4, 2>>(
        cs, Mb_wc_utf8mb4(), s, slen, t, tlen, t_is_prefix);
    default:
      DBUG_ASSERT(false);
    case 3:
      return my_strnncoll_uca<uca_scanner_900<Mb_wc_utf8mb4, 3>>(
        cs, Mb_wc_utf8mb4(), s, slen, t, tlen, t_is_prefix);
    }
  }

  Mb_wc_through_function_pointer mb_wc(cs);
  switch (cs->levels_for_compare)
  {
  case 1:
    return my_strnncoll_uca<uca_scanner_900<decltype(mb_wc), 1>>(
      cs, mb_wc, s, slen, t, tlen, t_is_prefix);
  case 2:
    return my_strnncoll_uca<uca_scanner_900<decltype(mb_wc), 2>>(
      cs, mb_wc, s, slen, t, tlen, t_is_prefix);
  default:
    DBUG_ASSERT(false);
  case 3:
    return my_strnncoll_uca<uca_scanner_900<decltype(mb_wc), 3>>(
      cs, mb_wc, s, slen, t, tlen, t_is_prefix);
  }
}

}  // extern "C"

template<class Mb_wc, int LEVELS_FOR_COMPARE>
static void my_hash_sort_uca_900_tmpl(const CHARSET_INFO *cs,
                                      const Mb_wc mb_wc,
                                      const uchar *s, size_t slen,
                                      ulong *n1, ulong *n2)
{
  ulong tmp1;
  ulong tmp2;

  slen= cs->cset->lengthsp(cs, (char*) s, slen);
  uca_scanner_900<Mb_wc, LEVELS_FOR_COMPARE> scanner(
    mb_wc, cs, &cs->uca->level[0], s, slen, slen);

  tmp1= *n1;
  tmp2= *n2;

  scanner.for_each_weight([&](int s_res) {
    tmp1^= (((tmp1 & 63) + tmp2) * (s_res >> 8)) + (tmp1 << 8);
    tmp2+= 3;
    tmp1^= (((tmp1 & 63) + tmp2) * (s_res & 0xFF)) + (tmp1 << 8);
    tmp2+= 3;
    return true;
  });

  *n1= tmp1;
  *n2= tmp2;
}

extern "C" {

static void my_hash_sort_uca_900(const CHARSET_INFO *cs,
                                 const uchar *s, size_t slen,
                                 ulong *n1, ulong *n2)
{
  if (cs->cset->mb_wc == my_mb_wc_utf8mb4_thunk)
  {
    switch (cs->levels_for_compare)
    {
    case 1:
      return my_hash_sort_uca_900_tmpl<Mb_wc_utf8mb4, 1>(
        cs, Mb_wc_utf8mb4(), s, slen, n1, n2);
    case 2:
      return my_hash_sort_uca_900_tmpl<Mb_wc_utf8mb4, 2>(
        cs, Mb_wc_utf8mb4(), s, slen, n1, n2);
    default:
      DBUG_ASSERT(false);
    case 3:
      return my_hash_sort_uca_900_tmpl<Mb_wc_utf8mb4, 3>(
        cs, Mb_wc_utf8mb4(), s, slen, n1, n2);
    }
  }

  Mb_wc_through_function_pointer mb_wc(cs);
  switch (cs->levels_for_compare)
  {
  case 1:
    return my_hash_sort_uca_900_tmpl<decltype(mb_wc), 1>(
      cs, mb_wc, s, slen, n1, n2);
  case 2:
    return my_hash_sort_uca_900_tmpl<decltype(mb_wc), 2>(
      cs, mb_wc, s, slen, n1, n2);
  default:
    DBUG_ASSERT(false);
  case 3:
    return my_hash_sort_uca_900_tmpl<decltype(mb_wc), 3>(
      cs, mb_wc, s, slen, n1, n2);
  }
}

}  // extern "C"

/**
  For each level, strip all 0x0001 weights from the end of the level.
  See the comments on my_strnxfrm_uca_900_tmpl for rationale.
*/
static inline uchar *strip_space_weights(const uchar *d0, uchar *dst)
{
  while (dst > d0 && dst[-2] == 0x00 && dst[-1] == 0x01)
  {
    dst-= sizeof(uint16);
  }

  return dst;
}

/**
  Since pad space takes so much space (and time) to write for complicated
  collations, strnxfrm for UCA 9.0.0 takes a more subtle approach to padding
  (MY_STRXFRM_PAD_WITH_SPACE) than actually adding spaces; it strips them
  from the right instead.

  However, we want to _preserve equality_ just as if we actually padded
  (for both 1-, 2-, and 3-level compare), and we want to maintain a
  _consistent_ ordering for strings that are not equal.
  (This ordering will be very similar to, but not identical to,
  the default DUCET ordering.)

  These are all the code points that could ever sort equal to SPACE:

      0020  ; [*0209.0020.0002] # SPACE
      00A0  ; [*0209.0020.001B] # NO-BREAK SPACE
      1680  ; [*0209.0020.0004] # OGHAM SPACE MARK
      2000  ; [*0209.0020.0004] # EN QUAD
      2001  ; [*0209.0020.0004] # EM QUAD
      2002  ; [*0209.0020.0004] # EN SPACE
      2003  ; [*0209.0020.0004] # EM SPACE
      2004  ; [*0209.0020.0004] # THREE-PER-EM SPACE
      2005  ; [*0209.0020.0004] # FOUR-PER-EM SPACE
      2006  ; [*0209.0020.0004] # SIX-PER-EM SPACE
      2007  ; [*0209.0020.001B] # FIGURE SPACE
      2008  ; [*0209.0020.0004] # PUNCTUATION SPACE
      2009  ; [*0209.0020.0004] # THIN SPACE
      200A  ; [*0209.0020.0004] # HAIR SPACE
      202F  ; [*0209.0020.001B] # NARROW NO-BREAK SPACE
      205F  ; [*0209.0020.0004] # MEDIUM MATHEMATICAL SPACE
      3000  ; [*0209.0020.0003] # IDEOGRAPHIC SPACE

  Now we make our only ordering change (this happens in uca9-dump.cc,
  so it is built into the tables). We make a tailoring so that the
  spaces sort lowest (nothing else ever has a weight of 0x0001):

      0020  ; [*0001.0020.0002] # SPACE
      00A0  ; [*0001.0020.001B] # NO-BREAK SPACE
      1680  ; [*0001.0020.0004] # OGHAM SPACE MARK
      2000  ; [*0001.0020.0004] # EN QUAD
      2001  ; [*0001.0020.0004] # EM QUAD
      2002  ; [*0001.0020.0004] # EN SPACE
      2003  ; [*0001.0020.0004] # EM SPACE
      2004  ; [*0001.0020.0004] # THREE-PER-EM SPACE
      2005  ; [*0001.0020.0004] # FOUR-PER-EM SPACE
      2006  ; [*0001.0020.0004] # SIX-PER-EM SPACE
      2007  ; [*0001.0020.001B] # FIGURE SPACE
      2008  ; [*0001.0020.0004] # PUNCTUATION SPACE
      2009  ; [*0001.0020.0004] # THIN SPACE
      200A  ; [*0001.0020.0004] # HAIR SPACE
      202F  ; [*0001.0020.001B] # NARROW NO-BREAK SPACE
      205F  ; [*0001.0020.0004] # MEDIUM MATHEMATICAL SPACE
      3000  ; [*0001.0020.0003] # IDEOGRAPHIC SPACE

  Now nothing sorts before SPACE, but SPACE still sorts equal to a few
  codepoints in primary and secondary weight. (SPACE sorts before everything
  if you consider all three weights.) In other words:

      [level end marker] < SPACE = [all other spaces] < *  [considering up to 2nd level]
      [level end marker] < SPACE < *                       [considering up to 3rd level]

  In PAD collations, everything is is conceptually the same length
  (by definition), so we can safely move things from the second group
  (ie., the group consisting of “all spaces” for first two levels,
  only SPACE for the third level) to the first by stripping them
  from the right. For convenience, let's change some more weights
  (this does not change the ordering):

      0020  ; [*0001.0001.0001] # SPACE
      00A0  ; [*0001.0001.001B] # NO-BREAK SPACE
      1680  ; [*0001.0001.0004] # OGHAM SPACE MARK
      2000  ; [*0001.0001.0004] # EN QUAD
      2001  ; [*0001.0001.0004] # EM QUAD
      2002  ; [*0001.0001.0004] # EN SPACE
      2003  ; [*0001.0001.0004] # EM SPACE
      2004  ; [*0001.0001.0004] # THREE-PER-EM SPACE
      2005  ; [*0001.0001.0004] # FOUR-PER-EM SPACE
      2006  ; [*0001.0001.0004] # SIX-PER-EM SPACE
      2007  ; [*0001.0001.001B] # FIGURE SPACE
      2008  ; [*0001.0001.0004] # PUNCTUATION SPACE
      2009  ; [*0001.0001.0004] # THIN SPACE
      200A  ; [*0001.0001.0004] # HAIR SPACE
      202F  ; [*0001.0001.001B] # NARROW NO-BREAK SPACE
      205F  ; [*0001.0001.0004] # MEDIUM MATHEMATICAL SPACE
      3000  ; [*0001.0001.0003] # IDEOGRAPHIC SPACE

  Now we can simply make the rule that every 0x0001 weight in front of
  a level marker gets stripped. This way, we keep equality of the
  different spaces on first and second level, but we keep the property
  that SPACE sorts first (and thus, _inequality_) on the third level.
  The only thing we have changed is that the SPACE group sorts before
  e.g. TAB (on the primary level), whereas earlier, it would sort later.
*/
template<class Mb_wc, int LEVELS_FOR_COMPARE>
static size_t my_strnxfrm_uca_900_tmpl(const CHARSET_INFO *cs,
                                       const Mb_wc mb_wc,
                                       uchar *dst, size_t dstlen, uint nweights,
                                       const uchar *src, size_t srclen,
                                       uint flags)
{
  uchar *d0= dst;
  uchar *dst_end= dst + dstlen;
  uca_scanner_900<Mb_wc, LEVELS_FOR_COMPARE> scanner(
    mb_wc, cs, &cs->uca->level[0], src, srclen, nweights);

  DBUG_ASSERT((dstlen % 2) == 0);
  if ((dstlen % 2) == 1)
  {
    // Emergency workaround for optimized mode.
    --dst_end;
  }

restart:
  if (dst != dst_end)
  {
    scanner.for_each_weight([&](int s_res) {
      if (LEVELS_FOR_COMPARE == 1)
        DBUG_ASSERT(s_res != 0);  // Level separator should never happen.
      else if (s_res == 0 && (flags & MY_STRXFRM_PAD_WITH_SPACE))
        dst= strip_space_weights(d0, dst);

      dst= store16be(dst, s_res);
      return (dst < dst_end);
    });
  }

  if (flags & MY_STRXFRM_PAD_WITH_SPACE)
  {
    uchar *nonspace_end= strip_space_weights(d0, dst);
    if (dst == dst_end && dst != nonspace_end)
    {
      /*
        If we went out of room for the output, there are two situations; either,
        the string has more non-space characters left for us, in which case we are
        done. However, if _only_ space weights (0x0001) in this level is space,
        we should strip them and restart! This is required for prefix correctness,
        ie. strnxfrm with an N-byte output buffer should give the same first
        N bytes as if running with an infinite output buffer.

        This is an edge case, so it's fine that it's not particularly fast.
      */
      for ( ;; )
      {
        int s_res= scanner.next();
        if (s_res == -1)
        {
          /*
            End of string without ever seeing anything except space.
            We need to strip the spaces we've added, and then we're done.
          */
          dst= nonspace_end;
          break;
        }
        if (s_res == 0)
        {
          /*
            End of level without seeing anything except space, but there are
            more levels. We need to strip the spaces we've added, write out
            the level separator, and then restart the normal loop writing weights
            (for the next level) as if nothing had happened.
          */
          dst= store16be(nonspace_end, s_res);
          goto restart;
        }
        if (s_res != 0x0001)
        {
          // A non-space weight; we should _not_ strip spaces, just end.
          break;
        }

        // Another space, so we still don't know; keep scanning the string.
        DBUG_ASSERT(s_res == 0x0001);
      }
    }
    else
    {
      dst= nonspace_end;
    }
  }

  if (flags & MY_STRXFRM_PAD_TO_MAXLEN)
  {
    memset(dst, 0, dst_end - dst);
    dst= dst_end;
  }

  my_strxfrm_desc_and_reverse(d0, dst, flags, 0);
  return dst - d0;
}

extern "C" {

static size_t my_strnxfrm_uca_900(const CHARSET_INFO *cs,
                                  uchar *dst, size_t dstlen, uint nweights,
                                  const uchar *src, size_t srclen, uint flags)
{
  if (cs->cset->mb_wc == my_mb_wc_utf8mb4_thunk)
  {
    switch (cs->levels_for_compare)
    {
    case 1:
      return my_strnxfrm_uca_900_tmpl<Mb_wc_utf8mb4, 1>(
        cs, Mb_wc_utf8mb4(), dst, dstlen, nweights, src, srclen, flags);
    case 2:
      return my_strnxfrm_uca_900_tmpl<Mb_wc_utf8mb4, 2>(
        cs, Mb_wc_utf8mb4(), dst, dstlen, nweights, src, srclen, flags);
    default:
      DBUG_ASSERT(false);
    case 3:
      return my_strnxfrm_uca_900_tmpl<Mb_wc_utf8mb4, 3>(
        cs, Mb_wc_utf8mb4(), dst, dstlen, nweights, src, srclen, flags);
    }
  }
  else
  {
    Mb_wc_through_function_pointer mb_wc(cs);
    switch (cs->levels_for_compare)
    {
    case 1:
      return my_strnxfrm_uca_900_tmpl<decltype(mb_wc), 1>(
        cs, mb_wc, dst, dstlen, nweights, src, srclen, flags);
    case 2:
      return my_strnxfrm_uca_900_tmpl<decltype(mb_wc), 2>(
        cs, mb_wc, dst, dstlen, nweights, src, srclen, flags);
    default:
      DBUG_ASSERT(false);
    case 3:
      return my_strnxfrm_uca_900_tmpl<decltype(mb_wc), 3>(
        cs, mb_wc, dst, dstlen, nweights, src, srclen, flags);
    }
  }
}

} // extern "C"


/*
  UCS2 optimized CHARSET_INFO compatible wrappers.
*/
extern "C" {
static int my_strnncoll_ucs2_uca(const CHARSET_INFO *cs,
                                 const uchar *s, size_t slen,
                                 const uchar *t, size_t tlen,
                                 my_bool t_is_prefix)
{
  Mb_wc_through_function_pointer mb_wc(cs);
  return my_strnncoll_uca<uca_scanner_any<decltype(mb_wc)>>(
    cs, mb_wc, s, slen, t, tlen, t_is_prefix);
}

static int my_strnncollsp_ucs2_uca(const CHARSET_INFO *cs,
                                   const uchar *s, size_t slen,
                                   const uchar *t, size_t tlen)
{
  Mb_wc_through_function_pointer mb_wc(cs);
  return my_strnncollsp_uca(cs, mb_wc, s, slen, t, tlen);
}

static void my_hash_sort_ucs2_uca(const CHARSET_INFO *cs,
                                  const uchar *s, size_t slen,
                                  ulong *n1, ulong *n2)
{
  Mb_wc_through_function_pointer mb_wc(cs);
  my_hash_sort_uca(cs, mb_wc, s, slen, n1, n2);
}

static size_t my_strnxfrm_ucs2_uca(const CHARSET_INFO *cs,
                                   uchar *dst, size_t dstlen, uint nweights,
                                   const uchar *src, size_t srclen, uint flags)
{
  Mb_wc_through_function_pointer mb_wc(cs);
  return my_strnxfrm_uca(cs, mb_wc, dst, dstlen, nweights, src, srclen, flags);
}
} // extern "C"

MY_COLLATION_HANDLER my_collation_ucs2_uca_handler =
{
  my_coll_init_uca,	/* init */
  my_strnncoll_ucs2_uca,
  my_strnncollsp_ucs2_uca,
  my_strnxfrm_ucs2_uca,
  my_strnxfrmlen_simple,
  my_like_range_generic,
  my_wildcmp_uca,
  NULL,
  my_instr_mb,
  my_hash_sort_ucs2_uca,
  my_propagate_complex
};

CHARSET_INFO my_charset_ucs2_unicode_ci=
{
    128,0,0,		/* number       */
    MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII,
    "ucs2",		/* cs name    */
    "ucs2_unicode_ci",	/* name         */
    "",			/* comment      */
    "",			/* tailoring    */
    NULL,		/* coll_param   */
    NULL,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    2,			/* mbminlen     */
    2,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_ucs2_handler,
    &my_collation_ucs2_uca_handler
};

CHARSET_INFO my_charset_ucs2_icelandic_uca_ci=
{
    129,0,0,		/* number       */
    MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII,
    "ucs2",		/* cs name    */
    "ucs2_icelandic_ci",/* name         */
    "",			/* comment      */
    icelandic,		/* tailoring    */
    NULL,		/* coll_param   */
    NULL,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    2,			/* mbminlen     */
    2,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_ucs2_handler,
    &my_collation_ucs2_uca_handler
};

CHARSET_INFO my_charset_ucs2_latvian_uca_ci=
{
    130,0,0,		/* number       */
    MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII,
    "ucs2",		/* cs name    */
    "ucs2_latvian_ci",	/* name         */
    "",			/* comment      */
    latvian,		/* tailoring    */
    NULL,		/* coll_param   */
    NULL,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    2,			/* mbminlen     */
    2,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_ucs2_handler,
    &my_collation_ucs2_uca_handler
};

CHARSET_INFO my_charset_ucs2_romanian_uca_ci=
{
    131,0,0,		/* number       */
    MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII,
    "ucs2",		/* cs name    */
    "ucs2_romanian_ci",	/* name         */
    "",			/* comment      */
    romanian,		/* tailoring    */
    NULL,		/* coll_param   */
    NULL,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    2,			/* mbminlen     */
    2,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_ucs2_handler,
    &my_collation_ucs2_uca_handler
};

CHARSET_INFO my_charset_ucs2_slovenian_uca_ci=
{
    132,0,0,		/* number       */
    MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII,
    "ucs2",		/* cs name    */
    "ucs2_slovenian_ci",/* name         */
    "",			/* comment      */
    slovenian,		/* tailoring    */
    NULL,		/* coll_param   */
    NULL,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    2,			/* mbminlen     */
    2,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_ucs2_handler,
    &my_collation_ucs2_uca_handler
};

CHARSET_INFO my_charset_ucs2_polish_uca_ci=
{
    133,0,0,		/* number       */
    MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII,
    "ucs2",		/* cs name    */
    "ucs2_polish_ci",	/* name         */
    "",			/* comment      */
    polish,		/* tailoring    */
    NULL,		/* coll_param   */
    NULL,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    2,			/* mbminlen     */
    2,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_ucs2_handler,
    &my_collation_ucs2_uca_handler
};

CHARSET_INFO my_charset_ucs2_estonian_uca_ci=
{
    134,0,0,		/* number       */
    MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII,
    "ucs2",		/* cs name    */
    "ucs2_estonian_ci",	/* name         */
    "",			/* comment      */
    estonian,		/* tailoring    */
    NULL,		/* coll_param   */
    NULL,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    2,			/* mbminlen     */
    2,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_ucs2_handler,
    &my_collation_ucs2_uca_handler
};

CHARSET_INFO my_charset_ucs2_spanish_uca_ci=
{
    135,0,0,		/* number       */
    MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII,
    "ucs2",		/* cs name    */
    "ucs2_spanish_ci",	/* name         */
    "",			/* comment      */
    spanish,		/* tailoring    */
    NULL,		/* coll_param   */
    NULL,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    2,			/* mbminlen     */
    2,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_ucs2_handler,
    &my_collation_ucs2_uca_handler
};

CHARSET_INFO my_charset_ucs2_swedish_uca_ci=
{
    136,0,0,		/* number       */
    MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII,
    "ucs2",		/* cs name    */
    "ucs2_swedish_ci",	/* name         */
    "",			/* comment      */
    swedish,		/* tailoring    */
    NULL,		/* coll_param   */
    NULL,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    2,			/* mbminlen     */
    2,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_ucs2_handler,
    &my_collation_ucs2_uca_handler
};

CHARSET_INFO my_charset_ucs2_turkish_uca_ci=
{
    137,0,0,		/* number       */
    MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII,
    "ucs2",		/* cs name    */
    "ucs2_turkish_ci",	/* name         */
    "",			/* comment      */
    turkish,		/* tailoring    */
    NULL,		/* coll_param   */
    NULL,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_turkish,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    2,			/* mbminlen     */
    2,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_ucs2_handler,
    &my_collation_ucs2_uca_handler
};

CHARSET_INFO my_charset_ucs2_czech_uca_ci=
{
    138,0,0,		/* number       */
    MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII,
    "ucs2",		/* cs name    */
    "ucs2_czech_ci",	/* name         */
    "",			/* comment      */
    czech,		/* tailoring    */
    NULL,		/* coll_param   */
    NULL,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    2,			/* mbminlen     */
    2,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_ucs2_handler,
    &my_collation_ucs2_uca_handler
};


CHARSET_INFO my_charset_ucs2_danish_uca_ci=
{
    139,0,0,		/* number       */
    MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII,
    "ucs2",		/* cs name    */
    "ucs2_danish_ci",	/* name         */
    "",			/* comment      */
    danish,		/* tailoring    */
    NULL,		/* coll_param   */
    NULL,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    2,			/* mbminlen     */
    2,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_ucs2_handler,
    &my_collation_ucs2_uca_handler
};

CHARSET_INFO my_charset_ucs2_lithuanian_uca_ci=
{
    140,0,0,		/* number       */
    MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII,
    "ucs2",		/* cs name    */
    "ucs2_lithuanian_ci",/* name         */
    "",			/* comment      */
    lithuanian,		/* tailoring    */
    NULL,		/* coll_param   */
    NULL,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    2,			/* mbminlen     */
    2,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_ucs2_handler,
    &my_collation_ucs2_uca_handler
};

CHARSET_INFO my_charset_ucs2_slovak_uca_ci=
{
    141,0,0,		/* number       */
    MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII,
    "ucs2",		/* cs name    */
    "ucs2_slovak_ci",	/* name         */
    "",			/* comment      */
    slovak,		/* tailoring    */
    NULL,		/* coll_param   */
    NULL,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    2,			/* mbminlen     */
    2,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_ucs2_handler,
    &my_collation_ucs2_uca_handler
};

CHARSET_INFO my_charset_ucs2_spanish2_uca_ci=
{
    142,0,0,		/* number       */
    MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII,
    "ucs2",		/* cs name    */
    "ucs2_spanish2_ci",	/* name         */
    "",			/* comment      */
    spanish2,		/* tailoring    */
    NULL,		/* coll_param   */
    NULL,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    2,			/* mbminlen     */
    2,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_ucs2_handler,
    &my_collation_ucs2_uca_handler
};


CHARSET_INFO my_charset_ucs2_roman_uca_ci=
{
    143,0,0,		/* number       */
    MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII,
    "ucs2",		/* cs name    */
    "ucs2_roman_ci",	/* name         */
    "",			/* comment      */
    roman,		/* tailoring    */
    NULL,		/* coll_param   */
    NULL,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    2,			/* mbminlen     */
    2,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_ucs2_handler,
    &my_collation_ucs2_uca_handler
};


CHARSET_INFO my_charset_ucs2_persian_uca_ci=
{
    144,0,0,		/* number       */
    MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII,
    "ucs2",		/* cs name    */
    "ucs2_persian_ci",	/* name         */
    "",			/* comment      */
    persian,		/* tailoring    */
    NULL,		/* coll_param   */
    NULL,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    2,			/* mbminlen     */
    2,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_ucs2_handler,
    &my_collation_ucs2_uca_handler
};


CHARSET_INFO my_charset_ucs2_esperanto_uca_ci=
{
    145,0,0,		/* number       */
    MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII,
    "ucs2",		/* cs name    */
    "ucs2_esperanto_ci",/* name         */
    "",			/* comment      */
    esperanto,		/* tailoring    */
    NULL,		/* coll_param   */
    NULL,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    2,			/* mbminlen     */
    2,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_ucs2_handler,
    &my_collation_ucs2_uca_handler
};


CHARSET_INFO my_charset_ucs2_hungarian_uca_ci=
{
    146,0,0,		/* number       */
    MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII,
    "ucs2",		/* cs name    */
    "ucs2_hungarian_ci",/* name         */
    "",			/* comment      */
    hungarian,		/* tailoring    */
    NULL,		/* coll_param   */
    NULL,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    2,			/* mbminlen     */
    2,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_ucs2_handler,
    &my_collation_ucs2_uca_handler
};


CHARSET_INFO my_charset_ucs2_sinhala_uca_ci=
{
    147,0,0,             /* number       */
    MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII,
    "ucs2",              /* csname    */
    "ucs2_sinhala_ci",   /* name         */
    "",                  /* comment      */
    sinhala,             /* tailoring    */
    NULL,		/* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    2,                   /* mbminlen     */
    2,                   /* mbmaxlen     */
    1,			 /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_ucs2_handler,
    &my_collation_ucs2_uca_handler
};


CHARSET_INFO my_charset_ucs2_german2_uca_ci=
{
    148,0,0,             /* number       */
    MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII,
    "ucs2",              /* csname    */
    "ucs2_german2_ci",   /* name         */
    "",                  /* comment      */
    german2,             /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    2,                   /* mbminlen     */
    2,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_ucs2_handler,
    &my_collation_ucs2_uca_handler
};


CHARSET_INFO my_charset_ucs2_croatian_uca_ci=
{
    149,0,0,             /* number       */
    MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII,
    "ucs2",              /* csname    */
    "ucs2_croatian_ci",  /* name         */
    "",                  /* comment      */
    croatian,            /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    2,                   /* mbminlen     */
    2,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_ucs2_handler,
    &my_collation_ucs2_uca_handler
};


CHARSET_INFO my_charset_ucs2_unicode_520_ci=
{
    150,0,0,            /* number       */
    MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII,
    "ucs2",             /* cs name      */
    "ucs2_unicode_520_ci",/* name       */
    "",                 /* comment      */
    "",                 /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,               /* ctype        */
    NULL,               /* to_lower     */
    NULL,               /* to_upper     */
    NULL,               /* sort_order   */
    &my_uca_v520,       /* uca          */
    NULL,               /* tab_to_uni   */
    NULL,               /* tab_from_uni */
    &my_unicase_unicode520,/* caseinfo  */
    NULL,               /* state_map    */
    NULL,               /* ident_map    */
    8,                  /* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    2,                  /* mbminlen     */
    2,                  /* mbmaxlen     */
    1,                  /* mbmaxlenlen  */
    9,                  /* min_sort_char */
    0xFFFF,             /* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_ucs2_handler,
    &my_collation_ucs2_uca_handler
};


CHARSET_INFO my_charset_ucs2_vietnamese_ci=
{
    151,0,0,             /* number       */
    MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII,
    "ucs2",              /* csname       */
    "ucs2_vietnamese_ci",/* name         */
    "",                  /* comment      */
    vietnamese,          /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    2,                   /* mbminlen     */
    2,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_ucs2_handler,
    &my_collation_ucs2_uca_handler
};




MY_COLLATION_HANDLER my_collation_any_uca_handler =
{
    my_coll_init_uca,	/* init */
    my_strnncoll_any_uca,
    my_strnncollsp_any_uca,
    my_strnxfrm_any_uca,
    my_strnxfrmlen_simple,
    my_like_range_mb,
    my_wildcmp_uca,
    my_strcasecmp_uca,
    my_instr_mb,
    my_hash_sort_any_uca,
    my_propagate_complex
};

MY_COLLATION_HANDLER my_collation_uca_900_handler =
{
    my_coll_init_uca,	/* init */
    my_strnncoll_uca_900,
    my_strnncollsp_uca_900,
    my_strnxfrm_uca_900,
    my_strnxfrmlen_simple,
    my_like_range_mb,
    my_wildcmp_uca,
    my_strcasecmp_uca,
    my_instr_mb,
    my_hash_sort_uca_900,
    my_propagate_complex
};


/*
  We consider bytes with code more than 127 as a letter.
  This garantees that word boundaries work fine with regular
  expressions. Note, there is no need to mark byte 255  as a
  letter, it is illegal byte in UTF8.
*/
static const uchar ctype_utf8[] = {
    0,
   32, 32, 32, 32, 32, 32, 32, 32, 32, 40, 40, 40, 40, 40, 32, 32,
   32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32,
   72, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
  132,132,132,132,132,132,132,132,132,132, 16, 16, 16, 16, 16, 16,
   16,129,129,129,129,129,129,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1, 16, 16, 16, 16, 16,
   16,130,130,130,130,130,130,  2,  2,  2,  2,  2,  2,  2,  2,  2,
    2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2, 16, 16, 16, 16, 32,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  0
};

extern MY_CHARSET_HANDLER my_charset_utf8_handler;

#define MY_CS_UTF8MB3_UCA_FLAGS  (MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE)

CHARSET_INFO my_charset_utf8_unicode_ci=
{
    192,0,0,		/* number       */
    MY_CS_UTF8MB3_UCA_FLAGS,/* flags    */
    "utf8",		/* cs name    */
    "utf8_unicode_ci",	/* name         */
    "",			/* comment      */
    "",			/* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    1,			/* mbminlen     */
    3,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_utf8_handler,
    &my_collation_any_uca_handler
};


CHARSET_INFO my_charset_utf8_icelandic_uca_ci=
{
    193,0,0,		/* number       */
    MY_CS_UTF8MB3_UCA_FLAGS,/* flags    */
    "utf8",		/* cs name    */
    "utf8_icelandic_ci",/* name         */
    "",			/* comment      */
    icelandic,		/* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    1,			/* mbminlen     */
    3,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_utf8_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8_latvian_uca_ci=
{
    194,0,0,		/* number       */
    MY_CS_UTF8MB3_UCA_FLAGS,/* flags    */
    "utf8",		/* cs name    */
    "utf8_latvian_ci",	/* name         */
    "",			/* comment      */
    latvian,		/* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    1,			/* mbminlen     */
    3,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_utf8_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8_romanian_uca_ci=
{
    195,0,0,		/* number       */
    MY_CS_UTF8MB3_UCA_FLAGS,/* flags    */
    "utf8",		/* cs name    */
    "utf8_romanian_ci",	/* name         */
    "",			/* comment      */
    romanian,		/* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    1,			/* mbminlen     */
    3,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_utf8_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8_slovenian_uca_ci=
{
    196,0,0,		/* number       */
    MY_CS_UTF8MB3_UCA_FLAGS,/* flags    */
    "utf8",		/* cs name    */
    "utf8_slovenian_ci",/* name         */
    "",			/* comment      */
    slovenian,		/* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    1,			/* mbminlen     */
    3,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_utf8_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8_polish_uca_ci=
{
    197,0,0,		/* number       */
    MY_CS_UTF8MB3_UCA_FLAGS,/* flags    */
    "utf8",		/* cs name    */
    "utf8_polish_ci",	/* name         */
    "",			/* comment      */
    polish,		/* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    1,			/* mbminlen     */
    3,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_utf8_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8_estonian_uca_ci=
{
    198,0,0,		/* number       */
    MY_CS_UTF8MB3_UCA_FLAGS,/* flags    */
    "utf8",		/* cs name    */
    "utf8_estonian_ci",	/* name         */
    "",			/* comment      */
    estonian,		/* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    1,			/* mbminlen     */
    3,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_utf8_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8_spanish_uca_ci=
{
    199,0,0,		/* number       */
    MY_CS_UTF8MB3_UCA_FLAGS,/* flags    */
    "utf8",		/* cs name    */
    "utf8_spanish_ci",	/* name         */
    "",			/* comment      */
    spanish,		/* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    1,			/* mbminlen     */
    3,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_utf8_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8_swedish_uca_ci=
{
    200,0,0,		/* number       */
    MY_CS_UTF8MB3_UCA_FLAGS,/* flags    */
    "utf8",		/* cs name    */
    "utf8_swedish_ci",	/* name         */
    "",			/* comment      */
    swedish,		/* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    1,			/* mbminlen     */
    3,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_utf8_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8_turkish_uca_ci=
{
    201,0,0,		/* number       */
    MY_CS_UTF8MB3_UCA_FLAGS,/* flags    */
    "utf8",		/* cs name    */
    "utf8_turkish_ci",	/* name         */
    "",			/* comment      */
    turkish,		/* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_turkish,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    2,                  /* caseup_multiply  */
    2,                  /* casedn_multiply  */
    1,			/* mbminlen     */
    3,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_utf8_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8_czech_uca_ci=
{
    202,0,0,		/* number       */
    MY_CS_UTF8MB3_UCA_FLAGS,/* flags    */
    "utf8",		/* cs name    */
    "utf8_czech_ci",	/* name         */
    "",			/* comment      */
    czech,		/* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    1,			/* mbminlen     */
    3,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_utf8_handler,
    &my_collation_any_uca_handler
};


CHARSET_INFO my_charset_utf8_danish_uca_ci=
{
    203,0,0,		/* number       */
    MY_CS_UTF8MB3_UCA_FLAGS,/* flags    */
    "utf8",		/* cs name    */
    "utf8_danish_ci",	/* name         */
    "",			/* comment      */
    danish,		/* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    1,			/* mbminlen     */
    3,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_utf8_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8_lithuanian_uca_ci=
{
    204,0,0,		/* number       */
    MY_CS_UTF8MB3_UCA_FLAGS,/* flags    */
    "utf8",		/* cs name    */
    "utf8_lithuanian_ci",/* name         */
    "",			/* comment      */
    lithuanian,		/* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    1,			/* mbminlen     */
    3,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_utf8_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8_slovak_uca_ci=
{
    205,0,0,		/* number       */
    MY_CS_UTF8MB3_UCA_FLAGS,/* flags    */
    "utf8",		/* cs name    */
    "utf8_slovak_ci",	/* name         */
    "",			/* comment      */
    slovak,		/* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    1,			/* mbminlen     */
    3,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_utf8_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8_spanish2_uca_ci=
{
    206,0,0,		/* number       */
    MY_CS_UTF8MB3_UCA_FLAGS,/* flags    */
    "utf8",		/* cs name    */
    "utf8_spanish2_ci",	/* name         */
    "",			/* comment      */
    spanish2,		/* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    1,			/* mbminlen     */
    3,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_utf8_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8_roman_uca_ci=
{
    207,0,0,		/* number       */
    MY_CS_UTF8MB3_UCA_FLAGS,/* flags    */
    "utf8",		/* cs name    */
    "utf8_roman_ci",	/* name         */
    "",			/* comment      */
    roman,		/* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    1,			/* mbminlen     */
    3,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_utf8_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8_persian_uca_ci=
{
    208,0,0,		/* number       */
    MY_CS_UTF8MB3_UCA_FLAGS,/* flags    */
    "utf8",		/* cs name    */
    "utf8_persian_ci",	/* name         */
    "",			/* comment      */
    persian,		/* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    1,			/* mbminlen     */
    3,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_utf8_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8_esperanto_uca_ci=
{
    209,0,0,		/* number       */
    MY_CS_UTF8MB3_UCA_FLAGS,/* flags    */
    "utf8",		/* cs name    */
    "utf8_esperanto_ci",/* name         */
    "",			/* comment      */
    esperanto,		/* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    1,			/* mbminlen     */
    3,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_utf8_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8_hungarian_uca_ci=
{
    210,0,0,		/* number       */
    MY_CS_UTF8MB3_UCA_FLAGS,/* flags    */
    "utf8",		/* cs name    */
    "utf8_hungarian_ci",/* name         */
    "",			/* comment      */
    hungarian,		/* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,		/* ctype        */
    NULL,		/* to_lower     */
    NULL,		/* to_upper     */
    NULL,		/* sort_order   */
    NULL,		/* uca          */
    NULL,		/* tab_to_uni   */
    NULL,		/* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,		/* state_map    */
    NULL,		/* ident_map    */
    8,			/* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    1,			/* mbminlen     */
    3,			/* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,			/* min_sort_char */
    0xFFFF,		/* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_utf8_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8_sinhala_uca_ci=
{
    211,0,0,             /* number       */
    MY_CS_UTF8MB3_UCA_FLAGS,/* flags    */
    "utf8",              /* cs name      */
    "utf8_sinhala_ci",   /* name         */
    "",                  /* comment      */
    sinhala,             /* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,          /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    1,                   /* mbminlen     */
    3,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf8_handler,
    &my_collation_any_uca_handler
};


CHARSET_INFO my_charset_utf8_german2_uca_ci=
{
    212,0,0,             /* number       */
    MY_CS_UTF8MB3_UCA_FLAGS,/* flags    */
    MY_UTF8MB3,          /* cs name      */
    MY_UTF8MB3 "_german2_ci",/* name    */
    "",                  /* comment      */
    german2,             /* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,          /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    1,                   /* mbminlen     */
    3,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf8_handler,
    &my_collation_any_uca_handler
};


CHARSET_INFO my_charset_utf8_croatian_uca_ci=
{
    213,0,0,             /* number       */
    MY_CS_UTF8MB3_UCA_FLAGS,/* flags    */
    MY_UTF8MB3,          /* cs name      */
    MY_UTF8MB3 "_croatian_ci",/* name    */
    "",                  /* comment      */
    croatian,            /* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,          /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    1,                   /* mbminlen     */
    3,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf8_handler,
    &my_collation_any_uca_handler
};


CHARSET_INFO my_charset_utf8_unicode_520_ci=
{
    214,0,0,             /* number       */
    MY_CS_UTF8MB3_UCA_FLAGS,/* flags     */
    MY_UTF8MB3,          /* csname       */
    MY_UTF8MB3 "_unicode_520_ci",/* name */
    "",                  /* comment      */
    "",                  /* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,          /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    &my_uca_v520,        /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_unicode520,/* caseinfo   */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    1,                   /* mbminlen     */
    3,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf8_handler,
    &my_collation_any_uca_handler
};


CHARSET_INFO my_charset_utf8_vietnamese_ci=
{
    215,0,0,             /* number       */
    MY_CS_UTF8MB3_UCA_FLAGS,/* flags     */
    MY_UTF8MB3,          /* cs name      */
    MY_UTF8MB3 "_vietnamese_ci",/* name  */
    "",                  /* comment      */
    vietnamese,          /* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,          /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    1,                   /* mbminlen     */
    3,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf8_handler,
    &my_collation_any_uca_handler
};

extern MY_CHARSET_HANDLER my_charset_utf8mb4_handler;

#define MY_CS_UTF8MB4_UCA_FLAGS  (MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_UNICODE_SUPPLEMENT)

CHARSET_INFO my_charset_utf8mb4_unicode_ci=
{
    224,0,0,             /* number       */
    MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
    MY_UTF8MB4,          /* csname      */
    MY_UTF8MB4 "_unicode_ci",/* name    */
    "",                  /* comment      */
    "",                  /* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,          /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    1,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf8mb4_handler,
    &my_collation_any_uca_handler
};


CHARSET_INFO my_charset_utf8mb4_icelandic_uca_ci=
{
    225,0,0,             /* number       */
    MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
    MY_UTF8MB4,          /* csname     */
    MY_UTF8MB4 "_icelandic_ci",/* name */
    "",                  /* comment      */
    icelandic,           /* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,          /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    1,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf8mb4_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8mb4_latvian_uca_ci=
{
    226,0,0,             /* number       */
    MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
    MY_UTF8MB4,          /* csname      */
    MY_UTF8MB4 "_latvian_ci", /*   name */
    "",                  /* comment      */
    latvian,             /* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,          /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    1,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf8mb4_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8mb4_romanian_uca_ci=
{
    227,0,0,             /* number       */
    MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
    MY_UTF8MB4,          /* csname      */
    MY_UTF8MB4 "_romanian_ci", /* name  */
    "",                  /* comment      */
    romanian,            /* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,          /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    1,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf8mb4_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8mb4_slovenian_uca_ci=
{
    228,0,0,             /* number       */
    MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
    MY_UTF8MB4,          /* csname      */
    MY_UTF8MB4 "_slovenian_ci",/* name  */
    "",                  /* comment      */
    slovenian,           /* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,          /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    1,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf8mb4_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8mb4_polish_uca_ci=
{
    229,0,0,             /* number       */
    MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
    MY_UTF8MB4,          /* csname      */
    MY_UTF8MB4 "_polish_ci", /* name    */
    "",                  /* comment      */
    polish,              /* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,          /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    1,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf8mb4_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8mb4_estonian_uca_ci=
{
    230,0,0,             /* number       */
    MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
    MY_UTF8MB4,          /* csname      */
    MY_UTF8MB4 "_estonian_ci", /*  name */
    "",                  /* comment      */
    estonian,            /* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,          /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    1,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf8mb4_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8mb4_spanish_uca_ci=
{
    231,0,0,             /* number       */
    MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
    MY_UTF8MB4,          /* csname      */
    MY_UTF8MB4 "_spanish_ci", /* name   */
    "",                  /* comment      */
    spanish,             /* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,          /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    1,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf8mb4_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8mb4_swedish_uca_ci=
{
    232,0,0,             /* number       */
    MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
    MY_UTF8MB4,          /* csname      */
    MY_UTF8MB4 "_swedish_ci", /* name   */
    "",                  /* comment      */
    swedish,             /* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,          /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    1,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf8mb4_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8mb4_turkish_uca_ci=
{
    233,0,0,             /* number       */
    MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
    MY_UTF8MB4,          /* csname      */
    MY_UTF8MB4 "_turkish_ci", /* name   */
    "",                  /* comment      */
    turkish,             /* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,          /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_turkish, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    2,                   /* caseup_multiply  */
    2,                   /* casedn_multiply  */
    1,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf8mb4_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8mb4_czech_uca_ci=
{
    234,0,0,             /* number       */
    MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
    MY_UTF8MB4,          /* csname      */
    MY_UTF8MB4 "_czech_ci", /* name     */
    "",                  /* comment      */
    czech,               /* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,          /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    1,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf8mb4_handler,
    &my_collation_any_uca_handler
};


CHARSET_INFO my_charset_utf8mb4_danish_uca_ci=
{
    235,0,0,             /* number       */
    MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
    MY_UTF8MB4,          /* csname      */
    MY_UTF8MB4 "_danish_ci", /* name    */
    "",                  /* comment      */
    danish,              /* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,          /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    1,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf8mb4_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8mb4_lithuanian_uca_ci=
{
    236,0,0,             /* number       */
    MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
    MY_UTF8MB4,          /* csname      */
    MY_UTF8MB4 "_lithuanian_ci",/* name */
    "",                  /* comment      */
    lithuanian,          /* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,          /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    1,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf8mb4_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8mb4_slovak_uca_ci=
{
    237,0,0,             /* number       */
    MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
    MY_UTF8MB4,          /* csname      */
    MY_UTF8MB4 "_slovak_ci", /* name    */
    "",                  /* comment      */
    slovak,              /* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,          /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    1,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf8mb4_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8mb4_spanish2_uca_ci=
{
    238,0,0,             /* number       */
    MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
    MY_UTF8MB4,          /* csname      */
    MY_UTF8MB4 "_spanish2_ci",      /* name */
    "",                  /* comment      */
    spanish2,            /* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,          /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    1,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf8mb4_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8mb4_roman_uca_ci=
{
    239,0,0,             /* number       */
    MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
    MY_UTF8MB4,          /* csname      */
    MY_UTF8MB4 "_roman_ci", /* name     */
    "",                  /* comment      */
    roman,               /* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,          /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    1,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf8mb4_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8mb4_persian_uca_ci=
{
    240,0,0,             /* number       */
    MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
    MY_UTF8MB4,          /* csname      */
    MY_UTF8MB4 "_persian_ci", /* name   */
    "",                  /* comment      */
    persian,             /* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,          /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    1,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf8mb4_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8mb4_esperanto_uca_ci=
{
    241,0,0,             /* number       */
    MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
    MY_UTF8MB4,          /* csname      */
    MY_UTF8MB4 "_esperanto_ci",/* name  */
    "",                  /* comment      */
    esperanto,           /* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,          /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    1,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf8mb4_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8mb4_hungarian_uca_ci=
{
    242,0,0,             /* number       */
    MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
    MY_UTF8MB4,          /* csname      */
    MY_UTF8MB4 "_hungarian_ci",/* name  */
    "",                  /* comment      */
    hungarian,           /* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,          /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    1,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf8mb4_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8mb4_sinhala_uca_ci=
{
    243,0,0,            /* number       */
    MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
    MY_UTF8MB4,         /* csname      */
    MY_UTF8MB4 "_sinhala_ci",/* name  */
    "",                 /* comment      */
    sinhala,            /* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,         /* ctype        */
    NULL,               /* to_lower     */
    NULL,               /* to_upper     */
    NULL,               /* sort_order   */
    NULL,               /* uca          */
    NULL,               /* tab_to_uni   */
    NULL,               /* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,               /* state_map    */
    NULL,               /* ident_map    */
    8,                  /* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    1,                  /* mbminlen      */
    4,                  /* mbmaxlen      */
    1,			/* mbmaxlenlen   */
    9,                  /* min_sort_char */
    0xFFFF,             /* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_utf8mb4_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8mb4_german2_uca_ci=
{
    244,0,0,            /* number       */
    MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
    MY_UTF8MB4,         /* csname      */
    MY_UTF8MB4 "_german2_ci",/* name  */
    "",                 /* comment      */
    german2,            /* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,         /* ctype        */
    NULL,               /* to_lower     */
    NULL,               /* to_upper     */
    NULL,               /* sort_order   */
    NULL,               /* uca          */
    NULL,               /* tab_to_uni   */
    NULL,               /* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,               /* state_map    */
    NULL,               /* ident_map    */
    8,                  /* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    1,                  /* mbminlen      */
    4,                  /* mbmaxlen      */
    1,			/* mbmaxlenlen   */
    9,                  /* min_sort_char */
    0xFFFF,             /* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_utf8mb4_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8mb4_croatian_uca_ci=
{
    245,0,0,            /* number       */
    MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
    MY_UTF8MB4,         /* csname      */
    MY_UTF8MB4 "_croatian_ci",/* name  */
    "",                 /* comment      */
    croatian,           /* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,         /* ctype        */
    NULL,               /* to_lower     */
    NULL,               /* to_upper     */
    NULL,               /* sort_order   */
    NULL,               /* uca          */
    NULL,               /* tab_to_uni   */
    NULL,               /* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,               /* state_map    */
    NULL,               /* ident_map    */
    8,                  /* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    1,                  /* mbminlen      */
    4,                  /* mbmaxlen      */
    1,			/* mbmaxlenlen   */
    9,                  /* min_sort_char */
    0xFFFF,             /* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_utf8mb4_handler,
    &my_collation_any_uca_handler
};

CHARSET_INFO my_charset_utf8mb4_unicode_520_ci=
{
    246,0,0,             /* number       */
    MY_CS_UTF8MB4_UCA_FLAGS,/* flags     */
    MY_UTF8MB4,          /* csname       */
    MY_UTF8MB4 "_unicode_520_ci",/* name */
    "",                  /* comment      */
    "",                  /* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,          /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    &my_uca_v520,        /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_unicode520,/* caseinfo   */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    1,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,			 /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0x10FFFF,            /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf8mb4_handler,
    &my_collation_any_uca_handler
};


CHARSET_INFO my_charset_utf8mb4_vietnamese_ci=
{
    247,0,0,            /* number       */
    MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
    MY_UTF8MB4,         /* csname       */
    MY_UTF8MB4 "_vietnamese_ci",/* name */
    "",                 /* comment      */
    vietnamese,         /* tailoring    */
    NULL,              	 /* coll_param   */
    ctype_utf8,         /* ctype        */
    NULL,               /* to_lower     */
    NULL,               /* to_upper     */
    NULL,               /* sort_order   */
    NULL,               /* uca          */
    NULL,               /* tab_to_uni   */
    NULL,               /* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,               /* state_map    */
    NULL,               /* ident_map    */
    8,                  /* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    1,                  /* mbminlen      */
    4,                  /* mbmaxlen      */
    1,			/* mbmaxlenlen   */
    9,                  /* min_sort_char */
    0xFFFF,             /* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_utf8mb4_handler,
    &my_collation_any_uca_handler
};





MY_COLLATION_HANDLER my_collation_utf32_uca_handler =
{
    my_coll_init_uca,        /* init */
    my_strnncoll_any_uca,
    my_strnncollsp_any_uca,
    my_strnxfrm_any_uca,
    my_strnxfrmlen_simple,
    my_like_range_generic,
    my_wildcmp_uca,
    NULL,
    my_instr_mb,
    my_hash_sort_any_uca,
    my_propagate_complex
};

extern MY_CHARSET_HANDLER my_charset_utf32_handler;

#define MY_CS_UTF32_UCA_FLAGS (MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII)

CHARSET_INFO my_charset_utf32_unicode_ci=
{
    160,0,0,             /* number       */
    MY_CS_UTF32_UCA_FLAGS,/* state       */
    "utf32",             /* csname       */
    "utf32_unicode_ci",  /* name         */
    "",                  /* comment      */
    "",                  /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    4,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf32_handler,
    &my_collation_utf32_uca_handler
};


CHARSET_INFO my_charset_utf32_icelandic_uca_ci=
{
    161,0,0,             /* number       */
    MY_CS_UTF32_UCA_FLAGS,/* state       */
    "utf32",             /* csname       */
    "utf32_icelandic_ci",/* name         */
    "",                  /* comment      */
    icelandic,           /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    4,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf32_handler,
    &my_collation_utf32_uca_handler
};

CHARSET_INFO my_charset_utf32_latvian_uca_ci=
{
    162,0,0,             /* number       */
    MY_CS_UTF32_UCA_FLAGS,/* state       */
    "utf32",             /* csname       */
    "utf32_latvian_ci",  /* name         */
    "",                  /* comment      */
    latvian,             /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    4,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf32_handler,
    &my_collation_utf32_uca_handler
};

CHARSET_INFO my_charset_utf32_romanian_uca_ci=
{
    163,0,0,             /* number       */
    MY_CS_UTF32_UCA_FLAGS,/* state       */
    "utf32",             /* csname       */
    "utf32_romanian_ci", /* name         */
    "",                  /* comment      */
    romanian,            /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    4,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf32_handler,
    &my_collation_utf32_uca_handler
};

CHARSET_INFO my_charset_utf32_slovenian_uca_ci=
{
    164,0,0,             /* number       */
    MY_CS_UTF32_UCA_FLAGS,/* state       */
    "utf32",             /* csname       */
    "utf32_slovenian_ci",/* name         */
    "",                  /* comment      */
    slovenian,           /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    4,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf32_handler,
    &my_collation_utf32_uca_handler
};

CHARSET_INFO my_charset_utf32_polish_uca_ci=
{
    165,0,0,             /* number       */
    MY_CS_UTF32_UCA_FLAGS,/* state       */
    "utf32",             /* csname       */
    "utf32_polish_ci",   /* name         */
    "",                  /* comment      */
    polish,              /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    4,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf32_handler,
    &my_collation_utf32_uca_handler
};

CHARSET_INFO my_charset_utf32_estonian_uca_ci=
{
    166,0,0,             /* number       */
    MY_CS_UTF32_UCA_FLAGS,/* state       */
    "utf32",             /* csname       */
    "utf32_estonian_ci", /* name         */
    "",                  /* comment      */
    estonian,            /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    4,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf32_handler,
    &my_collation_utf32_uca_handler
};

CHARSET_INFO my_charset_utf32_spanish_uca_ci=
{
    167,0,0,             /* number       */
    MY_CS_UTF32_UCA_FLAGS,/* state       */
    "utf32",             /* csname       */
    "utf32_spanish_ci",  /* name         */
    "",                  /* comment      */
    spanish,             /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    4,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf32_handler,
    &my_collation_utf32_uca_handler
};

CHARSET_INFO my_charset_utf32_swedish_uca_ci=
{
    168,0,0,             /* number       */
    MY_CS_UTF32_UCA_FLAGS,/* state       */
    "utf32",             /* csname       */
    "utf32_swedish_ci",  /* name         */
    "",                  /* comment      */
    swedish,             /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    4,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf32_handler,
    &my_collation_utf32_uca_handler
};

CHARSET_INFO my_charset_utf32_turkish_uca_ci=
{
    169,0,0,             /* number       */
    MY_CS_UTF32_UCA_FLAGS,/* state       */
    "utf32",             /* csname       */
    "utf32_turkish_ci",  /* name         */
    "",                  /* comment      */
    turkish,             /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_turkish, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    4,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf32_handler,
    &my_collation_utf32_uca_handler
};

CHARSET_INFO my_charset_utf32_czech_uca_ci=
{
    170,0,0,             /* number       */
    MY_CS_UTF32_UCA_FLAGS,/* state       */
    "utf32",             /* csname       */
    "utf32_czech_ci",    /* name         */
    "",                  /* comment      */
    czech,               /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    4,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf32_handler,
    &my_collation_utf32_uca_handler
};


CHARSET_INFO my_charset_utf32_danish_uca_ci=
{
    171,0,0,             /* number       */
    MY_CS_UTF32_UCA_FLAGS,/* state       */
    "utf32",             /* csname       */
    "utf32_danish_ci",   /* name         */
    "",                  /* comment      */
    danish,              /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    4,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf32_handler,
    &my_collation_utf32_uca_handler
};

CHARSET_INFO my_charset_utf32_lithuanian_uca_ci=
{
    172,0,0,             /* number       */
    MY_CS_UTF32_UCA_FLAGS,/* state       */
    "utf32",             /* csname       */
    "utf32_lithuanian_ci",/* name        */
    "",                  /* comment      */
    lithuanian,          /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    4,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf32_handler,
    &my_collation_utf32_uca_handler
};

CHARSET_INFO my_charset_utf32_slovak_uca_ci=
{
    173,0,0,             /* number       */
    MY_CS_UTF32_UCA_FLAGS,/* state       */
    "utf32",             /* csname       */
    "utf32_slovak_ci",   /* name         */
    "",                  /* comment      */
    slovak,              /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    4,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf32_handler,
    &my_collation_utf32_uca_handler
};

CHARSET_INFO my_charset_utf32_spanish2_uca_ci=
{
    174,0,0,             /* number       */
    MY_CS_UTF32_UCA_FLAGS,/* state       */
    "utf32",             /* csname       */
    "utf32_spanish2_ci", /* name         */
    "",                  /* comment      */
    spanish2,            /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    4,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf32_handler,
    &my_collation_utf32_uca_handler
};

CHARSET_INFO my_charset_utf32_roman_uca_ci=
{
    175,0,0,             /* number       */
    MY_CS_UTF32_UCA_FLAGS,/* state       */
    "utf32",             /* csname       */
    "utf32_roman_ci",    /* name         */
    "",                  /* comment      */
    roman,               /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    4,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf32_handler,
    &my_collation_utf32_uca_handler
};

CHARSET_INFO my_charset_utf32_persian_uca_ci=
{
    176,0,0,             /* number       */
    MY_CS_UTF32_UCA_FLAGS,/* state       */
    "utf32",             /* csname       */
    "utf32_persian_ci",  /* name         */
    "",                  /* comment      */
    persian,             /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    4,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf32_handler,
    &my_collation_utf32_uca_handler
};

CHARSET_INFO my_charset_utf32_esperanto_uca_ci=
{
    177,0,0,             /* number       */
    MY_CS_UTF32_UCA_FLAGS,/* state       */
    "utf32",             /* csname       */
    "utf32_esperanto_ci",/* name         */
    "",                  /* comment      */
    esperanto,           /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    4,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf32_handler,
    &my_collation_utf32_uca_handler
};

CHARSET_INFO my_charset_utf32_hungarian_uca_ci=
{
    178,0,0,             /* number       */
    MY_CS_UTF32_UCA_FLAGS,/* state       */
    "utf32",             /* csname       */
    "utf32_hungarian_ci",/* name         */
    "",                  /* comment      */
    hungarian,           /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    4,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf32_handler,
    &my_collation_utf32_uca_handler
};

CHARSET_INFO my_charset_utf32_sinhala_uca_ci=
{
    179,0,0,            /* number       */
    MY_CS_UTF32_UCA_FLAGS,/* state      */
    "utf32",            /* csname       */
    "utf32_sinhala_ci", /* name         */
    "",                 /* comment      */
    sinhala,            /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,               /* ctype        */
    NULL,               /* to_lower     */
    NULL,               /* to_upper     */
    NULL,               /* sort_order   */
    NULL,               /* uca          */
    NULL,               /* tab_to_uni   */
    NULL,               /* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,               /* state_map    */
    NULL,               /* ident_map    */
    8,                  /* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    4,                  /* mbminlen     */
    4,                  /* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,                  /* min_sort_char */
    0xFFFF,             /* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_utf32_handler,
    &my_collation_utf32_uca_handler
};

CHARSET_INFO my_charset_utf32_german2_uca_ci=
{
    180,0,0,            /* number       */
    MY_CS_UTF32_UCA_FLAGS,/* state      */
    "utf32",            /* csname      */
    "utf32_german2_ci", /* name         */
    "",                 /* comment      */
    german2,            /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,               /* ctype        */
    NULL,               /* to_lower     */
    NULL,               /* to_upper     */
    NULL,               /* sort_order   */
    NULL,               /* uca          */
    NULL,               /* tab_to_uni   */
    NULL,               /* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,               /* state_map    */
    NULL,               /* ident_map    */
    8,                  /* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    4,                  /* mbminlen     */
    4,                  /* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,                  /* min_sort_char */
    0xFFFF,             /* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_utf32_handler,
    &my_collation_utf32_uca_handler
};

CHARSET_INFO my_charset_utf32_croatian_uca_ci=
{
    181,0,0,            /* number       */
    MY_CS_UTF32_UCA_FLAGS,/* state      */
    "utf32",            /* csname      */
    "utf32_croatian_ci", /* name        */
    "",                 /* comment      */
    croatian,           /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,               /* ctype        */
    NULL,               /* to_lower     */
    NULL,               /* to_upper     */
    NULL,               /* sort_order   */
    NULL,               /* uca          */
    NULL,               /* tab_to_uni   */
    NULL,               /* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,               /* state_map    */
    NULL,               /* ident_map    */
    8,                  /* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    4,                  /* mbminlen     */
    4,                  /* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,                  /* min_sort_char */
    0xFFFF,             /* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_utf32_handler,
    &my_collation_utf32_uca_handler
};

CHARSET_INFO my_charset_utf32_unicode_520_ci=
{
    182,0,0,             /* number       */
    MY_CS_UTF32_UCA_FLAGS,/* stat e      */
    "utf32",             /* csname       */
    "utf32_unicode_520_ci",/* name       */
    "",                  /* comment      */
    "",                  /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    &my_uca_v520,        /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_unicode520,/* caseinfo   */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    4,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0x10FFFF,            /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf32_handler,
    &my_collation_utf32_uca_handler
};


CHARSET_INFO my_charset_utf32_vietnamese_ci=
{
    183,0,0,            /* number       */
    MY_CS_UTF32_UCA_FLAGS,/* state      */
    "utf32",            /* csname       */
    "utf32_vietnamese_ci",/* name       */
    "",                 /* comment      */
    vietnamese,         /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,               /* ctype        */
    NULL,               /* to_lower     */
    NULL,               /* to_upper     */
    NULL,               /* sort_order   */
    NULL,               /* uca          */
    NULL,               /* tab_to_uni   */
    NULL,               /* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,               /* state_map    */
    NULL,               /* ident_map    */
    8,                  /* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    4,                  /* mbminlen     */
    4,                  /* mbmaxlen     */
    1,			/* mbmaxlenlen  */
    9,                  /* min_sort_char */
    0xFFFF,             /* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_utf32_handler,
    &my_collation_utf32_uca_handler
};






MY_COLLATION_HANDLER my_collation_utf16_uca_handler =
{
    my_coll_init_uca,        /* init */
    my_strnncoll_any_uca,
    my_strnncollsp_any_uca,
    my_strnxfrm_any_uca,
    my_strnxfrmlen_simple,
    my_like_range_generic,
    my_wildcmp_uca,
    NULL,
    my_instr_mb,
    my_hash_sort_any_uca,
    my_propagate_complex
};

extern MY_CHARSET_HANDLER my_charset_utf16_handler;

#define MY_CS_UTF16_UCA_FLAGS (MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII)

CHARSET_INFO my_charset_utf16_unicode_ci=
{
    101,0,0,             /* number       */
    MY_CS_UTF16_UCA_FLAGS,/* state       */
    "utf16",             /* csname       */
    "utf16_unicode_ci",  /* name         */
    "",                  /* comment      */
    "",                  /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    2,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf16_handler,
    &my_collation_utf16_uca_handler
};


CHARSET_INFO my_charset_utf16_icelandic_uca_ci=
{
    102,0,0,             /* number       */
    MY_CS_UTF16_UCA_FLAGS,/* state       */
    "utf16",             /* csname       */
    "utf16_icelandic_ci",/* name         */
    "",                  /* comment      */
    icelandic,           /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    2,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf16_handler,
    &my_collation_utf16_uca_handler
};

CHARSET_INFO my_charset_utf16_latvian_uca_ci=
{
    103,0,0,             /* number       */
    MY_CS_UTF16_UCA_FLAGS,/* state       */
    "utf16",             /* cs name      */
    "utf16_latvian_ci",  /* name         */
    "",                  /* comment      */
    latvian,             /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    2,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf16_handler,
    &my_collation_utf16_uca_handler
};

CHARSET_INFO my_charset_utf16_romanian_uca_ci=
{
    104,0,0,             /* number       */
    MY_CS_UTF16_UCA_FLAGS,/* state       */
    "utf16",             /* cs name      */
    "utf16_romanian_ci", /* name         */
    "",                  /* comment      */
    romanian,            /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    2,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf16_handler,
    &my_collation_utf16_uca_handler
};

CHARSET_INFO my_charset_utf16_slovenian_uca_ci=
{
    105,0,0,             /* number       */
    MY_CS_UTF16_UCA_FLAGS,/* state       */
    "utf16",             /* cs name      */
    "utf16_slovenian_ci",/* name         */
    "",                  /* comment      */
    slovenian,           /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    2,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf16_handler,
    &my_collation_utf16_uca_handler
};

CHARSET_INFO my_charset_utf16_polish_uca_ci=
{
    106,0,0,             /* number       */
    MY_CS_UTF16_UCA_FLAGS,/* state       */
    "utf16",             /* cs name      */
    "utf16_polish_ci",   /* name         */
    "",                  /* comment      */
    polish,              /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    2,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf16_handler,
    &my_collation_utf16_uca_handler
};

CHARSET_INFO my_charset_utf16_estonian_uca_ci=
{
    107,0,0,             /* number       */
    MY_CS_UTF16_UCA_FLAGS,/* state       */
    "utf16",             /* cs name      */
    "utf16_estonian_ci", /* name         */
    "",                  /* comment      */
    estonian,            /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    2,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf16_handler,
    &my_collation_utf16_uca_handler
};

CHARSET_INFO my_charset_utf16_spanish_uca_ci=
{
    108,0,0,             /* number       */
    MY_CS_UTF16_UCA_FLAGS,/* state       */
    "utf16",             /* cs name      */
    "utf16_spanish_ci",  /* name         */
    "",                  /* comment      */
    spanish,             /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    2,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf16_handler,
    &my_collation_utf16_uca_handler
};

CHARSET_INFO my_charset_utf16_swedish_uca_ci=
{
    109,0,0,             /* number       */
    MY_CS_UTF16_UCA_FLAGS,/* state       */
    "utf16",             /* cs name      */
    "utf16_swedish_ci",  /* name         */
    "",                  /* comment      */
    swedish,             /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    2,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf16_handler,
    &my_collation_utf16_uca_handler
};

CHARSET_INFO my_charset_utf16_turkish_uca_ci=
{
    110,0,0,             /* number       */
    MY_CS_UTF16_UCA_FLAGS,/* state       */
    "utf16",             /* cs name      */
    "utf16_turkish_ci",  /* name         */
    "",                  /* comment      */
    turkish,             /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_turkish, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    2,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf16_handler,
    &my_collation_utf16_uca_handler
};

CHARSET_INFO my_charset_utf16_czech_uca_ci=
{
    111,0,0,             /* number       */
    MY_CS_UTF16_UCA_FLAGS,/* state       */
    "utf16",             /* cs name      */
    "utf16_czech_ci",    /* name         */
    "",                  /* comment      */
    czech,               /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    2,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf16_handler,
    &my_collation_utf16_uca_handler
};


CHARSET_INFO my_charset_utf16_danish_uca_ci=
{
    112,0,0,             /* number       */
    MY_CS_UTF16_UCA_FLAGS,/* state       */
    "utf16",             /* cs name      */
    "utf16_danish_ci",   /* name         */
    "",                  /* comment      */
    danish,              /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    2,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf16_handler,
    &my_collation_utf16_uca_handler
};

CHARSET_INFO my_charset_utf16_lithuanian_uca_ci=
{
    113,0,0,             /* number       */
    MY_CS_UTF16_UCA_FLAGS,/* state       */
    "utf16",             /* cs name      */
    "utf16_lithuanian_ci",/* name        */
    "",                  /* comment      */
    lithuanian,          /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    2,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf16_handler,
    &my_collation_utf16_uca_handler
};

CHARSET_INFO my_charset_utf16_slovak_uca_ci=
{
    114,0,0,             /* number       */
    MY_CS_UTF16_UCA_FLAGS,/* state       */
    "utf16",             /* cs name      */
    "utf16_slovak_ci",   /* name         */
    "",                  /* comment      */
    slovak,              /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    NULL,                /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_default, /* caseinfo     */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    2,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0xFFFF,              /* max_sort_char */
    ' ',                 /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf16_handler,
    &my_collation_utf16_uca_handler
};

CHARSET_INFO my_charset_utf16_spanish2_uca_ci=
{
    115,0,0,            /* number       */
    MY_CS_UTF16_UCA_FLAGS,/* state      */
    "utf16",            /* cs name      */
    "utf16_spanish2_ci",/* name         */
    "",                 /* comment      */
    spanish2,           /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,               /* ctype        */
    NULL,               /* to_lower     */
    NULL,               /* to_upper     */
    NULL,               /* sort_order   */
    NULL,               /* uca          */
    NULL,               /* tab_to_uni   */
    NULL,               /* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,               /* state_map    */
    NULL,               /* ident_map    */
    8,                  /* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    2,                  /* mbminlen      */
    4,                  /* mbmaxlen      */
    1,			/* mbmaxlenlen   */
    9,                  /* min_sort_char */
    0xFFFF,             /* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_utf16_handler,
    &my_collation_utf16_uca_handler
};

CHARSET_INFO my_charset_utf16_roman_uca_ci=
{
    116,0,0,            /* number       */
    MY_CS_UTF16_UCA_FLAGS,/* state      */
    "utf16",            /* cs name      */
    "utf16_roman_ci",   /* name         */
    "",                 /* comment      */
    roman,              /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,               /* ctype        */
    NULL,               /* to_lower     */
    NULL,               /* to_upper     */
    NULL,               /* sort_order   */
    NULL,               /* uca          */
    NULL,               /* tab_to_uni   */
    NULL,               /* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,               /* state_map    */
    NULL,               /* ident_map    */
    8,                  /* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    2,                  /* mbminlen      */
    4,                  /* mbmaxlen      */
    1,			/* mbmaxlenlen   */
    9,                  /* min_sort_char */
    0xFFFF,             /* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_utf16_handler,
    &my_collation_utf16_uca_handler
};

CHARSET_INFO my_charset_utf16_persian_uca_ci=
{
    117,0,0,            /* number       */
    MY_CS_UTF16_UCA_FLAGS,/* state      */
    "utf16",            /* cs name      */
    "utf16_persian_ci", /* name         */
    "",                 /* comment      */
    persian,            /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,               /* ctype        */
    NULL,               /* to_lower     */
    NULL,               /* to_upper     */
    NULL,               /* sort_order   */
    NULL,               /* uca          */
    NULL,               /* tab_to_uni   */
    NULL,               /* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,               /* state_map    */
    NULL,               /* ident_map    */
    8,                  /* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    2,                  /* mbminlen      */
    4,                  /* mbmaxlen      */
    1,			/* mbmaxlenlen   */
    9,                  /* min_sort_char */
    0xFFFF,             /* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_utf16_handler,
    &my_collation_utf16_uca_handler
};

CHARSET_INFO my_charset_utf16_esperanto_uca_ci=
{
    118,0,0,            /* number       */
    MY_CS_UTF16_UCA_FLAGS,/* state      */
    "utf16",            /* cs name      */
    "utf16_esperanto_ci",/* name        */
    "",                 /* comment      */
    esperanto,          /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,               /* ctype        */
    NULL,               /* to_lower     */
    NULL,               /* to_upper     */
    NULL,               /* sort_order   */
    NULL,               /* uca          */
    NULL,               /* tab_to_uni   */
    NULL,               /* tab_from_uni */
    &my_unicase_default,/* caseinfo     */
    NULL,               /* state_map    */
    NULL,               /* ident_map    */
    8,                  /* strxfrm_multiply */
    1,                  /* caseup_multiply  */
    1,                  /* casedn_multiply  */
    2,                  /* mbminlen      */
    4,                  /* mbmaxlen      */
    1,			/* mbmaxlenlen   */
    9,                  /* min_sort_char */
    0xFFFF,             /* max_sort_char */
    ' ',                /* pad char      */
    0,                  /* escape_with_backslash_is_dangerous */
    1,                  /* levels_for_compare */
    1,                  /* levels_for_order   */
    &my_charset_utf16_handler,
    &my_collation_utf16_uca_handler
};

CHARSET_INFO my_charset_utf16_hungarian_uca_ci=
{
    119,0,0,           /* number       */
    MY_CS_UTF16_UCA_FLAGS,/* state     */
    "utf16",           /* cs name      */
    "utf16_hungarian_ci",/* name       */
    "",                /* comment      */
    hungarian,         /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,              /* ctype        */
    NULL,              /* to_lower     */
    NULL,              /* to_upper     */
    NULL,              /* sort_order   */
    NULL,              /* uca          */
    NULL,              /* tab_to_uni   */
    NULL,              /* tab_from_uni */
    &my_unicase_default,/* caseinfo    */
    NULL,              /* state_map    */
    NULL,              /* ident_map    */
    8,                 /* strxfrm_multiply */
    1,                 /* caseup_multiply  */
    1,                 /* casedn_multiply  */
    2,                 /* mbminlen      */
    4,                 /* mbmaxlen      */
    1,                 /* mbmaxlenlen   */
    9,                 /* min_sort_char */
    0xFFFF,            /* max_sort_char */
    ' ',               /* pad char      */
    0,                 /* escape_with_backslash_is_dangerous */
    1,                 /* levels_for_compare */
    1,                 /* levels_for_order   */
    &my_charset_utf16_handler,
    &my_collation_utf16_uca_handler
};

CHARSET_INFO my_charset_utf16_sinhala_uca_ci=
{
    120,0,0,           /* number       */
    MY_CS_UTF16_UCA_FLAGS,/* state     */
    "utf16",           /* cs name      */
    "utf16_sinhala_ci",/* name         */
    "",                /* comment      */
    sinhala,           /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,              /* ctype        */
    NULL,              /* to_lower     */
    NULL,              /* to_upper     */
    NULL,              /* sort_order   */
    NULL,              /* uca          */
    NULL,              /* tab_to_uni   */
    NULL,              /* tab_from_uni */
    &my_unicase_default,/* caseinfo    */
    NULL,              /* state_map    */
    NULL,              /* ident_map    */
    8,                 /* strxfrm_multiply */
    1,                 /* caseup_multiply  */
    1,                 /* casedn_multiply  */
    2,                 /* mbminlen     */
    4,                 /* mbmaxlen     */
    1,                 /* mbmaxlenlen  */
    9,                 /* min_sort_char */
    0xFFFF,            /* max_sort_char */
    ' ',               /* pad char      */
    0,                 /* escape_with_backslash_is_dangerous */
    1,                 /* levels_for_compare */
    1,                 /* levels_for_order   */
    &my_charset_utf16_handler,
    &my_collation_utf16_uca_handler
};

CHARSET_INFO my_charset_utf16_german2_uca_ci=
{
    121,0,0,           /* number       */
    MY_CS_UTF16_UCA_FLAGS,/* state     */
    "utf16",           /* cs name    */
    "utf16_german2_ci",/* name         */
    "",                /* comment      */
    german2,           /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,              /* ctype        */
    NULL,              /* to_lower     */
    NULL,              /* to_upper     */
    NULL,              /* sort_order   */
    NULL,              /* uca          */
    NULL,              /* tab_to_uni   */
    NULL,              /* tab_from_uni */
    &my_unicase_default,/* caseinfo    */
    NULL,              /* state_map    */
    NULL,              /* ident_map    */
    8,                 /* strxfrm_multiply */
    1,                 /* caseup_multiply  */
    1,                 /* casedn_multiply  */
    2,                 /* mbminlen     */
    4,                 /* mbmaxlen     */
    1,                 /* mbmaxlenlen  */
    9,                 /* min_sort_char */
    0xFFFF,            /* max_sort_char */
    ' ',               /* pad char      */
    0,                 /* escape_with_backslash_is_dangerous */
    1,                 /* levels_for_compare */
    1,                 /* levels_for_order   */
    &my_charset_utf16_handler,
    &my_collation_utf16_uca_handler
};


CHARSET_INFO my_charset_utf16_croatian_uca_ci=
{
    122,0,0,           /* number       */
    MY_CS_UTF16_UCA_FLAGS,/* state     */
    "utf16",           /* cs name    */
    "utf16_croatian_ci",/* name         */
    "",                /* comment      */
    croatian,           /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,              /* ctype        */
    NULL,              /* to_lower     */
    NULL,              /* to_upper     */
    NULL,              /* sort_order   */
    NULL,              /* uca          */
    NULL,              /* tab_to_uni   */
    NULL,              /* tab_from_uni */
    &my_unicase_default,/* caseinfo    */
    NULL,              /* state_map    */
    NULL,              /* ident_map    */
    8,                 /* strxfrm_multiply */
    1,                 /* caseup_multiply  */
    1,                 /* casedn_multiply  */
    2,                 /* mbminlen     */
    4,                 /* mbmaxlen     */
    1,                 /* mbmaxlenlen  */
    9,                 /* min_sort_char */
    0xFFFF,            /* max_sort_char */
    ' ',               /* pad char      */
    0,                 /* escape_with_backslash_is_dangerous */
    1,                 /* levels_for_compare */
    1,                 /* levels_for_order   */
    &my_charset_utf16_handler,
    &my_collation_utf16_uca_handler
};


CHARSET_INFO my_charset_utf16_unicode_520_ci=
{
    123,0,0,             /* number       */
    MY_CS_COMPILED|MY_CS_STRNXFRM|MY_CS_UNICODE|MY_CS_NONASCII,
    "utf16",             /* csname       */
    "utf16_unicode_520_ci",/* name       */
    "",                  /* comment      */
    "",                  /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,                /* ctype        */
    NULL,                /* to_lower     */
    NULL,                /* to_upper     */
    NULL,                /* sort_order   */
    &my_uca_v520,        /* uca          */
    NULL,                /* tab_to_uni   */
    NULL,                /* tab_from_uni */
    &my_unicase_unicode520,/* caseinfo   */
    NULL,                /* state_map    */
    NULL,                /* ident_map    */
    8,                   /* strxfrm_multiply */
    1,                   /* caseup_multiply  */
    1,                   /* casedn_multiply  */
    2,                   /* mbminlen     */
    4,                   /* mbmaxlen     */
    1,                   /* mbmaxlenlen  */
    9,                   /* min_sort_char */
    0x10FFFF,            /* max_sort_char */
    0x20,                /* pad char      */
    0,                   /* escape_with_backslash_is_dangerous */
    1,                   /* levels_for_compare */
    1,                   /* levels_for_order   */
    &my_charset_utf16_handler,
    &my_collation_utf16_uca_handler
};


CHARSET_INFO my_charset_utf16_vietnamese_ci=
{
    124,0,0,           /* number       */
    MY_CS_UTF16_UCA_FLAGS,/* state     */
    "utf16",           /* cs name      */
    "utf16_vietnamese_ci",/* name      */
    "",                /* comment      */
    vietnamese,        /* tailoring    */
    NULL,              	 /* coll_param   */
    NULL,              /* ctype        */
    NULL,              /* to_lower     */
    NULL,              /* to_upper     */
    NULL,              /* sort_order   */
    NULL,              /* uca          */
    NULL,              /* tab_to_uni   */
    NULL,              /* tab_from_uni */
    &my_unicase_default,/* caseinfo    */
    NULL,              /* state_map    */
    NULL,              /* ident_map    */
    8,                 /* strxfrm_multiply */
    1,                 /* caseup_multiply  */
    1,                 /* casedn_multiply  */
    2,                 /* mbminlen     */
    4,                 /* mbmaxlen     */
    1,                 /* mbmaxlenlen  */
    9,                 /* min_sort_char */
    0xFFFF,            /* max_sort_char */
    ' ',               /* pad char      */
    0,                 /* escape_with_backslash_is_dangerous */
    1,                 /* levels_for_compare */
    1,                 /* levels_for_order   */
    &my_charset_utf16_handler,
    &my_collation_utf16_uca_handler
};




MY_COLLATION_HANDLER my_collation_gb18030_uca_handler =
{
    my_coll_init_uca,   /* init */
    my_strnncoll_any_uca,
    my_strnncollsp_any_uca,
    my_strnxfrm_any_uca,
    my_strnxfrmlen_simple,
    my_like_range_mb,
    my_wildcmp_uca,
    NULL,
    my_instr_mb,
    my_hash_sort_any_uca,
    my_propagate_complex
};

/**
  The array used for "type of characters" bit mask for each
  character. The ctype[0] is reserved for EOF(-1), so we use
  ctype[(char)+1]. Also refer to strings/CHARSET_INFO.txt
*/
static const uchar ctype_gb18030[257]=
{
   0,                                   /* For standard library */
   32, 32, 32, 32, 32, 32, 32, 32, 32, 40, 40, 40, 40, 40, 32, 32,
   32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32,
   72, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16,
  132,132,132,132,132,132,132,132,132,132, 16, 16, 16, 16, 16, 16,
   16,129,129,129,129,129,129,  1,  1,  1,  1,  1,  1,  1,  1,  1,
    1,  1,  1,  1,  1,  1,  1,  1,  1,  1,  1, 16, 16, 16, 16, 16,
   16,130,130,130,130,130,130,  2,  2,  2,  2,  2,  2,  2,  2,  2,
    2,  2,  2,  2,  2,  2,  2,  2,  2,  2,  2, 16, 16, 16, 16, 32,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,
    3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  3,  0
};

extern MY_CHARSET_HANDLER my_charset_gb18030_uca_handler;

CHARSET_INFO my_charset_gb18030_unicode_520_ci=
{
    250, 0, 0,         /* number        */
    MY_CS_COMPILED | MY_CS_STRNXFRM | MY_CS_NONASCII, /* state         */
    "gb18030",         /* cs name       */
    "gb18030_unicode_520_ci",/* name        */
    "",                /* comment       */
    "",                /* tailoring     */
    NULL,              	 /* coll_param   */
    ctype_gb18030,     /* ctype         */
    NULL,              /* lower         */
    NULL,              /* UPPER         */
    NULL,              /* sort order    */
    &my_uca_v520,      /* uca           */
    NULL,              /* tab_to_uni    */
    NULL,              /* tab_from_uni  */
    &my_unicase_unicode520,/* caseinfo     */
    NULL,              /* state_map     */
    NULL,              /* ident_map     */
    8,                 /* strxfrm_multiply */
    2,                 /* caseup_multiply  */
    2,                 /* casedn_multiply  */
    1,                 /* mbminlen      */
    4,                 /* mbmaxlen      */
    2,                 /* mbmaxlenlen   */
    0,                 /* min_sort_char */
    0xE3329A35,        /* max_sort_char */
    ' ',               /* pad char      */
    0,                 /* escape_with_backslash_is_dangerous */
    1,                 /* levels_for_compare */
    1,                 /* levels_for_order   */
    &my_charset_gb18030_uca_handler,
    &my_collation_gb18030_uca_handler
};


CHARSET_INFO my_charset_utf8mb4_0900_ai_ci=
{
  255, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_0900_ai_ci",/* name */
  "",                 /* comment      */
  NULL,         /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca_900      */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  8,                  /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  1,                  /* levels_for_compare */
  1,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_de_pb_0900_ai_ci=
{
  256, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_de_pb_0900_ai_ci",/* name */
  "",                 /* comment      */
  de_pb_cldr_29,      /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca_900          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  8,                  /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  1,                  /* levels_for_compare */
  1,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_is_0900_ai_ci=
{
  257, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_is_0900_ai_ci",/* name */
  "",                 /* comment      */
  is_cldr_29,         /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  8,                  /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  1,                  /* levels_for_compare */
  1,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_lv_0900_ai_ci=
{
  258, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_lv_0900_ai_ci",/* name */
  "",                 /* comment      */
  lv_cldr_29,         /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  8,                  /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  1,                  /* levels_for_compare */
  1,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_ro_0900_ai_ci=
{
  259, 0, 0,          /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_ro_0900_ai_ci",/* name */
  "",                 /* comment      */
  ro_cldr_29,         /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  8,                  /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  1,                  /* levels_for_compare */
  1,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_sl_0900_ai_ci=
{
  260, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_sl_0900_ai_ci",/* name */
  "",                 /* comment      */
  sl_cldr_29,         /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  8,                  /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  1,                  /* levels_for_compare */
  1,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_pl_0900_ai_ci=
{
  261, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_pl_0900_ai_ci",/* name */
  "",                 /* comment      */
  pl_cldr_29,         /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  8,                  /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  1,                  /* levels_for_compare */
  1,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_et_0900_ai_ci=
{
  262, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_et_0900_ai_ci",/* name */
  "",                 /* comment      */
  et_cldr_29,         /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  8,                  /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  1,                  /* levels_for_compare */
  1,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_es_0900_ai_ci=
{
  263, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_es_0900_ai_ci",/* name */
  "",                 /* comment      */
  spanish,            /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  8,                  /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  1,                  /* levels_for_compare */
  1,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_sv_0900_ai_ci=
{
  264, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_sv_0900_ai_ci",/* name */
  "",                 /* comment      */
  sv_cldr_29,         /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  8,                  /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  1,                  /* levels_for_compare */
  1,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_tr_0900_ai_ci=
{
  265, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_tr_0900_ai_ci",/* name */
  "",                 /* comment      */
  tr_cldr_29,         /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  8,                  /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  1,                  /* levels_for_compare */
  1,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_cs_0900_ai_ci=
{
  266, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_cs_0900_ai_ci",/* name */
  "",                 /* comment      */
  cs_cldr_29,         /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  8,                  /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  1,                  /* levels_for_compare */
  1,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_da_0900_ai_ci=
{
  267, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_da_0900_ai_ci",/* name */
  "",                 /* comment      */
  da_cldr_29,         /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  8,                  /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  1,                  /* levels_for_compare */
  1,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_lt_0900_ai_ci=
{
  268, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_lt_0900_ai_ci",/* name */
  "",                 /* comment      */
  lt_cldr_29,         /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  8,                  /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  1,                  /* levels_for_compare */
  1,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_sk_0900_ai_ci=
{
  269, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_sk_0900_ai_ci",/* name */
  "",                 /* comment      */
  sk_cldr_29,         /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  8,                  /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  1,                  /* levels_for_compare */
  1,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_es_trad_0900_ai_ci=
{
  270, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_es_trad_0900_ai_ci",/* name */
  "",                 /* comment      */
  es_trad_cldr_29,    /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  8,                  /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  1,                  /* levels_for_compare */
  1,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_la_0900_ai_ci=
{
  271, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_la_0900_ai_ci",/* name */
  "",                 /* comment      */
  roman,              /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  8,                  /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  1,                  /* levels_for_compare */
  1,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

#if 0
CHARSET_INFO my_charset_utf8mb4_fa_0900_ai_ci=
{
  272, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_fa_0900_ai_ci",/* name */
  "",                 /* comment      */
  fa_cldr_29,         /* tailoring    */
  &fa_coll_param,     /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  8,                  /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  1,                  /* levels_for_compare */
  1,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};
#endif

CHARSET_INFO my_charset_utf8mb4_eo_0900_ai_ci=
{
  273, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_eo_0900_ai_ci",/* name */
  "",                 /* comment      */
  esperanto,          /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  8,                  /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  1,                  /* levels_for_compare */
  1,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_hu_0900_ai_ci=
{
  274, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_hu_0900_ai_ci",/* name */
  "",                 /* comment      */
  hu_cldr_29,         /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  8,                  /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  1,                  /* levels_for_compare */
  1,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_hr_0900_ai_ci=
{
  275, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_hr_0900_ai_ci",/* name */
  "",                 /* comment      */
  hr_cldr_29,         /* tailoring    */
  &hr_coll_param,     /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  8,                  /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  1,                  /* levels_for_compare */
  1,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

#if 0
CHARSET_INFO my_charset_utf8mb4_si_0900_ai_ci=
{
  276, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_si_0900_ai_ci",/* name */
  "",                 /* comment      */
  si_cldr_29,         /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  8,                  /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  1,                  /* levels_for_compare */
  1,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};
#endif

CHARSET_INFO my_charset_utf8mb4_vi_0900_ai_ci=
{
  277, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_vi_0900_ai_ci",/* name */
  "",                 /* comment      */
  vi_cldr_29,         /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  8,                  /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  1,                  /* levels_for_compare */
  1,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_0900_as_cs=
{
  278, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS|MY_CS_CSSORT,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_0900_as_cs",/* name */
  "",                 /* comment      */
  NULL,               /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  24,                 /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  3,                  /* levels_for_compare */
  3,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_de_pb_0900_as_cs=
{
  279, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS|MY_CS_CSSORT,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_de_pb_0900_as_cs",/* name */
  "",                 /* comment      */
  de_pb_cldr_29,/* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  24,                 /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  3,                  /* levels_for_compare */
  3,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_is_0900_as_cs=
{
  280, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS|MY_CS_CSSORT,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_is_0900_as_cs",/* name */
  "",                 /* comment      */
  is_cldr_29,         /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  24,                 /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  3,                  /* levels_for_compare */
  3,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_lv_0900_as_cs=
{
  281, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS|MY_CS_CSSORT,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_lv_0900_as_cs",/* name */
  "",                 /* comment      */
  lv_cldr_29,         /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  24,                 /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  3,                  /* levels_for_compare */
  3,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_ro_0900_as_cs=
{
  282, 0, 0,          /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS|MY_CS_CSSORT,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_ro_0900_as_cs",/* name */
  "",                 /* comment      */
  ro_cldr_29,         /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  24,                 /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  3,                  /* levels_for_compare */
  3,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_sl_0900_as_cs=
{
  283, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS|MY_CS_CSSORT,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_sl_0900_as_cs",/* name */
  "",                 /* comment      */
  sl_cldr_29,         /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  24,                 /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  3,                  /* levels_for_compare */
  3,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_pl_0900_as_cs=
{
  284, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS|MY_CS_CSSORT,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_pl_0900_as_cs",/* name */
  "",                 /* comment      */
  pl_cldr_29,         /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  24,                 /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  3,                  /* levels_for_compare */
  3,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_et_0900_as_cs=
{
  285, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS|MY_CS_CSSORT,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_et_0900_as_cs",/* name */
  "",                 /* comment      */
  et_cldr_29,         /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  24,                 /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  3,                  /* levels_for_compare */
  3,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_es_0900_as_cs=
{
  286, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS|MY_CS_CSSORT,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_es_0900_as_cs",/* name */
  "",                 /* comment      */
  spanish,            /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  24,                 /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  3,                  /* levels_for_compare */
  3,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_sv_0900_as_cs=
{
  287, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS|MY_CS_CSSORT,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_sv_0900_as_cs",/* name */
  "",                 /* comment      */
  sv_cldr_29,         /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  24,                 /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  3,                  /* levels_for_compare */
  3,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_tr_0900_as_cs=
{
  288, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS|MY_CS_CSSORT,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_tr_0900_as_cs",/* name */
  "",                 /* comment      */
  tr_cldr_29,         /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  24,                 /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  3,                  /* levels_for_compare */
  3,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_cs_0900_as_cs=
{
  289, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS|MY_CS_CSSORT,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_cs_0900_as_cs",/* name */
  "",                 /* comment      */
  cs_cldr_29,         /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  24,                 /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  3,                  /* levels_for_compare */
  3,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_da_0900_as_cs=
{
  290, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS|MY_CS_CSSORT,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_da_0900_as_cs",/* name */
  "",                 /* comment      */
  da_cldr_29,         /* tailoring    */
  &da_coll_param,     /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  24,                 /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  3,                  /* levels_for_compare */
  3,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_lt_0900_as_cs=
{
  291, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS|MY_CS_CSSORT,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_lt_0900_as_cs",/* name */
  "",                 /* comment      */
  lt_cldr_29,         /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  24,                 /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  3,                  /* levels_for_compare */
  3,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_sk_0900_as_cs=
{
  292, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS|MY_CS_CSSORT,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_sk_0900_as_cs",/* name */
  "",                 /* comment      */
  sk_cldr_29,         /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  24,                 /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  3,                  /* levels_for_compare */
  3,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_es_trad_0900_as_cs=
{
  293, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS|MY_CS_CSSORT,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_es_trad_0900_as_cs",/* name */
  "",                 /* comment      */
  es_trad_cldr_29,/* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  24,                 /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  3,                  /* levels_for_compare */
  3,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_la_0900_as_cs=
{
  294, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS|MY_CS_CSSORT,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_la_0900_as_cs",/* name */
  "",                 /* comment      */
  roman,              /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  24,                 /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  3,                  /* levels_for_compare */
  3,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

#if 0
CHARSET_INFO my_charset_utf8mb4_fa_0900_as_cs=
{
  295, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS|MY_CS_CSSORT,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_fa_0900_as_cs",/* name */
  "",                 /* comment      */
  fa_cldr_29,         /* tailoring    */
  &fa_coll_param,     /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  24,                 /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  3,                  /* levels_for_compare */
  3,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};
#endif

CHARSET_INFO my_charset_utf8mb4_eo_0900_as_cs=
{
  296, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS|MY_CS_CSSORT,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_eo_0900_as_cs",/* name */
  "",                 /* comment      */
  esperanto,          /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  24,                 /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  3,                  /* levels_for_compare */
  3,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_hu_0900_as_cs=
{
  297, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS|MY_CS_CSSORT,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_hu_0900_as_cs",/* name */
  "",                 /* comment      */
  hu_cldr_29,         /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  24,                 /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  3,                  /* levels_for_compare */
  3,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

CHARSET_INFO my_charset_utf8mb4_hr_0900_as_cs=
{
  298, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS|MY_CS_CSSORT,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_hr_0900_as_cs",/* name */
  "",                 /* comment      */
  hr_cldr_29,         /* tailoring    */
  &hr_coll_param,     /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  24,                 /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  3,                  /* levels_for_compare */
  3,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};

#if 0
CHARSET_INFO my_charset_utf8mb4_si_0900_as_cs=
{
  299, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS|MY_CS_CSSORT,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_si_0900_as_cs",/* name */
  "",                 /* comment      */
  si_cldr_29,         /* tailoring    */
  NULL,               /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  24,                 /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  3,                  /* levels_for_compare */
  3,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};
#endif

CHARSET_INFO my_charset_utf8mb4_vi_0900_as_cs=
{
  300, 0, 0,            /* number       */
  MY_CS_UTF8MB4_UCA_FLAGS|MY_CS_CSSORT,/* state    */
  MY_UTF8MB4,         /* csname       */
  MY_UTF8MB4 "_vi_0900_as_cs",/* name */
  "",                 /* comment      */
  vi_cldr_29,         /* tailoring    */
  &vi_coll_param,     /* coll_param   */
  ctype_utf8,         /* ctype        */
  NULL,               /* to_lower     */
  NULL,               /* to_upper     */
  NULL,               /* sort_order   */
  &my_uca_v900,       /* uca          */
  NULL,               /* tab_to_uni   */
  NULL,               /* tab_from_uni */
  &my_unicase_unicode900,/* caseinfo     */
  NULL,               /* state_map    */
  NULL,               /* ident_map    */
  24,                 /* strxfrm_multiply */
  1,                  /* caseup_multiply  */
  1,                  /* casedn_multiply  */
  1,                  /* mbminlen      */
  4,                  /* mbmaxlen      */
  1,                  /* mbmaxlenlen   */
  32,                 /* min_sort_char */
  0x10FFFF,           /* max_sort_char */
  ' ',                /* pad char      */
  0,                  /* escape_with_backslash_is_dangerous */
  3,                  /* levels_for_compare */
  3,                  /* levels_for_order   */
  &my_charset_utf8mb4_handler,
  &my_collation_uca_900_handler
};
