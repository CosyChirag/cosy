#include <studio.h>

int main() {
    long a = 3;
    long b = 2;
    long c = a + b;
    a = 4;

    .debug_line: line number info for a single cu
Source lines (from CU-DIE at .debug_info offset 0x0000000b):

            NS new statement, BB new basic block, ET end of text sequence
            PE prologue end, EB epilogue begin
            IS=val ISA number, DI=val discriminator value
<pc>        [lno,col] NS BB ET PE EB IS= DI= uri: "filepath"
0x00400670  [   1, 0] NS uri: "/path/to/test.cpp"
0x00400676  [   2,10] NS PE
0x0040067e  [   3,10] NS
0x00400686  [   4,14] NS
0x0040068a  [   4,16]
0x0040068e  [   4,10]
0x00400692  [   5, 7] NS
0x0040069a  [   6, 1] NS
0x0040069c  [   6, 1] NS ET

0x0040067e  [   3,10] NS

.debug_info

COMPILE_UNIT<header overall offset = 0x00000000>:
< 0><0x0000000b>  DW_TAG_compile_unit
                    DW_AT_producer              clang version 3.9.1 (tags/RELEASE_391/final)
                    DW_AT_language              DW_LANG_C_plus_plus
                    DW_AT_name                  /path/to/variable.cpp
                    DW_AT_stmt_list             0x00000000
                    DW_AT_comp_dir              /path/to
                    DW_AT_low_pc                0x00400670
                    DW_AT_high_pc               0x0040069c

LOCAL_SYMBOLS:
< 1><0x0000002e>    DW_TAG_subprogram
                      DW_AT_low_pc                0x00400670
                      DW_AT_high_pc               0x0040069c
                      DW_AT_frame_base            DW_OP_reg6
                      DW_AT_name                  main
                      DW_AT_decl_file             0x00000001 /path/to/variable.cpp
                      DW_AT_decl_line             0x00000001
                      DW_AT_type                  <0x00000077>
                      DW_AT_external              yes(1)
< 2><0x0000004c>      DW_TAG_variable
                        DW_AT_location              DW_OP_fbreg -8
                        DW_AT_name                  a
                        DW_AT_decl_file             0x00000001 /path/to/variable.cpp
                        DW_AT_decl_line             0x00000002
                        DW_AT_type                  <0x0000007e>
< 2><0x0000005a>      DW_TAG_variable
                        DW_AT_location              DW_OP_fbreg -16
                        DW_AT_name                  b
                        DW_AT_decl_file             0x00000001 /path/to/variable.cpp
                        DW_AT_decl_line             0x00000003
                        DW_AT_type                  <0x0000007e>
< 2><0x00000068>      DW_TAG_variable
                        DW_AT_location              DW_OP_fbreg -24
                        DW_AT_name                  c
                        DW_AT_decl_file             0x00000001 /path/to/variable.cpp
                        DW_AT_decl_line             0x00000004
                        DW_AT_type                  <0x0000007e>
< 1><0x00000077>    DW_TAG_base_type
                      DW_AT_name                  int
                      DW_AT_encoding              DW_ATE_signed
                      DW_AT_byte_size             0x00000004
< 1><0x0000007e>    DW_TAG_base_type
                      DW_AT_name                  long int
                      DW_AT_encoding              DW_ATE_signed
                      DW_AT_byte_size             0x00000008

for each compile unit:
    if the pc is between DW_AT_low_pc and DW_AT_high_pc:
        for each function in the compile unit:
            if the pc is between DW_AT_low_pc and DW_AT_high_pc:
                return function information

< 1><0x0000002e>    DW_TAG_subprogram
                      DW_AT_low_pc                0x00400670
                      DW_AT_high_pc               0x0040069c
                      DW_AT_frame_base            DW_OP_reg6
                      DW_AT_name                  main
                      DW_AT_decl_file             0x00000001 /path/to/variable.cpp
                      DW_AT_decl_line             0x00000001
                      DW_AT_type                  <0x00000077>
                      DW_AT_external              yes(1)

0x00400670  [   1, 0] NS uri: "/path/to/variable.cpp"

0x00400676  [   2,10] NS PE

DW_AT_location              DW_OP_fbreg -8

DW_AT_frame_base            DW_OP_reg6


< 2><0x0000004c>      DW_TAG_variable
                        DW_AT_name                  a
                        DW_AT_type                  <0x0000007e>

                        < 1><0x0000007e>    DW_TAG_base_type
                      DW_AT_name                  long int
                      DW_AT_encoding              DW_ATE_signed
                      DW_AT_byte_size             0x00000008

DW_AT_frame_base            <loclist at offset 0x00000000 with 4 entries follows>
 low-off : 0x00000000 addr  0x00400696 high-off  0x00000001 addr 0x00400697>DW_OP_breg7+8
 low-off : 0x00000001 addr  0x00400697 high-off  0x00000004 addr 0x0040069a>DW_OP_breg7+16
 low-off : 0x00000004 addr  0x0040069a high-off  0x00000031 addr 0x004006c7>DW_OP_breg6+16
 low-off : 0x00000031 addr  0x004006c7 high-off  0x00000032 addr 0x004006c8>DW_OP_breg7+8


} return 0;