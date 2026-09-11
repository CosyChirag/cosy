struct test{
    int i;
    float j;
    int k[42];
    test* next;
};

< 1><0x0000002a>    DW_TAG_structure_type
                      DW_AT_name                  "test"
                      DW_AT_byte_size             0x000000b8
                      DW_AT_decl_file             0x00000001 test.cpp
                      DW_AT_decl_line             0x00000001

                      < 2><0x00000032>      DW_TAG_member
                        DW_AT_name                  "i"
                        DW_AT_type                  <0x00000063>
                        DW_AT_decl_file             0x00000001 test.cpp
                        DW_AT_decl_line             0x00000002
                        DW_AT_data_member_location  0
< 2><0x0000003e>      DW_TAG_member
                        DW_AT_name                  "j"
                        DW_AT_type                  <0x0000006a>
                        DW_AT_decl_file             0x00000001 test.cpp
                        DW_AT_decl_line             0x00000003
                        DW_AT_data_member_location  4
< 2><0x0000004a>      DW_TAG_member
                        DW_AT_name                  "k"
                        DW_AT_type                  <0x00000071>
                        DW_AT_decl_file             0x00000001 test.cpp
                        DW_AT_decl_line             0x00000004
                        DW_AT_data_member_location  8
< 2><0x00000056>      DW_TAG_member
                        DW_AT_name                  "next"
                        DW_AT_type                  <0x00000084>
                        DW_AT_decl_file             0x00000001 test.cpp
                        DW_AT_decl_line             0x00000005
                        DW_AT_data_member_location  176(as signed = -80)


                        < 1><0x00000063>    DW_TAG_base_type
                      DW_AT_name                  "int"
                      DW_AT_encoding              DW_ATE_signed
                      DW_AT_byte_size             0x00000004
< 1><0x0000006a>    DW_TAG_base_type
                      DW_AT_name                  "float"
                      DW_AT_encoding              DW_ATE_float
                      DW_AT_byte_size             0x00000004
< 1><0x00000071>    DW_TAG_array_type
                      DW_AT_type                  <0x00000063>
< 2><0x00000076>      DW_TAG_subrange_type
                        DW_AT_type                  <0x0000007d>
                        DW_AT_count                 0x0000002a
< 1><0x0000007d>    DW_TAG_base_type
                      DW_AT_name                  "sizetype"
                      DW_AT_byte_size             0x00000008
                      DW_AT_encoding              DW_ATE_unsigned
< 1><0x00000084>    DW_TAG_pointer_type
                      DW_AT_type                  <0x0000002a>


                      class ptrace_expr_context : public dwarf::expr_context {
public:
    ptrace_expr_context (pid_t pid) : m_pid{pid} {}

    dwarf::taddr reg (unsigned regnum) override {
        return get_register_value_from_dwarf_register(m_pid, regnum);
    }

    dwarf::taddr pc() override {
        struct user_regs_struct regs;
        ptrace(PTRACE_GETREGS, m_pid, nullptr, &regs);
        return regs.rip;
    }

    dwarf::taddr deref_size (dwarf::taddr address, unsigned size) override {
        //TODO take into account size
        return ptrace(PTRACE_PEEKDATA, m_pid, address, nullptr);
    }

private:
    pid_t m_pid;
};

void debugger::read_variables() {
    using namespace dwarf;

    auto func = get_function_from_pc(get_offset_pc());

    //...
}
    for (const auto& die : func) {
        if (die.tag == DW_TAG::variable) {
            //...
        }
    }
                auto loc_val = die[DW_AT::location];

                            if (loc_val.get_type() == value::type::exprloc) {
                ptrace_expr_context context {m_pid};
                auto result = loc_val.as_exprloc().evaluate(&context);

                switch (result.location_type) {
                case expr_result::type::address:
                {
                    auto value = read_memory(result.value);
                    std::cout << at_name(die) << " (0x" << std::hex << result.value << ") = "
                              << value << std::endl;
                }

                case expr_result::type::reg:
                {
                    auto value = get_register_value_from_dwarf_register(m_pid, result.value);
                    std::cout << at_name(die) << " (reg " << result.value << ") = "
                              << value << std::endl;
                    break;
                }

                default:
                    throw std::runtime_error{"Unhandled variable location"};
                }

                    else if(is_prefix(command, "variables")) {
        read_variables();
    }
    