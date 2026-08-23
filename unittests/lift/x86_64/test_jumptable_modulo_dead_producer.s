// A valid `%140` producer elsewhere in the function cannot authorize a raw
// selector that overwrites it before the table address is formed.  Keep this
// deliberately unresolved function in its own object so whole-object emitter
// tests for the other modulo negatives remain independent.

        .text
        .globl  jt_modulo_u140_dead_producer
        .type   jt_modulo_u140_dead_producer,@function
jt_modulo_u140_dead_producer:
        movl    %edi, %eax
        movl    %eax, %ecx
        shrl    $2, %ecx
        imulq   $0x3a83a83b, %rcx, %rcx
        shrq    $35, %rcx
        imull   $140, %ecx, %ecx
        subl    %ecx, %eax
        movl    %esi, %eax
        leaq    .Lmod140_dead_table(%rip), %rcx
        movslq  (%rcx,%rax,4), %rax
        addq    %rcx, %rax
        jmpq    *%rax

.Lmod140_dead_case_0:
        movl    $6000, %eax
        retq

.Lmod140_dead_case_1:
        movl    $6001, %eax
        retq

.Lmod140_dead_case_2:
        movl    $6002, %eax
        retq

.Lmod140_dead_case_3:
        movl    $6003, %eax
        retq

.Lmod140_dead_case_4:
        movl    $6004, %eax
        retq

.Lmod140_dead_case_5:
        movl    $6005, %eax
        retq

.Lmod140_dead_case_6:
        movl    $6006, %eax
        retq

.Lmod140_dead_case_7:
        movl    $6007, %eax
        retq

.Lmod140_dead_case_8:
        movl    $6008, %eax
        retq

.Lmod140_dead_case_9:
        movl    $6009, %eax
        retq

.Lmod140_dead_case_10:
        movl    $6010, %eax
        retq

.Lmod140_dead_case_11:
        movl    $6011, %eax
        retq

.Lmod140_dead_case_12:
        movl    $6012, %eax
        retq

.Lmod140_dead_case_13:
        movl    $6013, %eax
        retq

.Lmod140_dead_case_14:
        movl    $6014, %eax
        retq

.Lmod140_dead_case_15:
        movl    $6015, %eax
        retq

.Lmod140_dead_case_16:
        movl    $6016, %eax
        retq

.Lmod140_dead_case_17:
        movl    $6017, %eax
        retq

.Lmod140_dead_case_18:
        movl    $6018, %eax
        retq

.Lmod140_dead_case_19:
        movl    $6019, %eax
        retq

.Lmod140_dead_case_20:
        movl    $6020, %eax
        retq

.Lmod140_dead_case_21:
        movl    $6021, %eax
        retq

.Lmod140_dead_case_22:
        movl    $6022, %eax
        retq

.Lmod140_dead_case_23:
        movl    $6023, %eax
        retq

.Lmod140_dead_case_24:
        movl    $6024, %eax
        retq

.Lmod140_dead_case_25:
        movl    $6025, %eax
        retq

.Lmod140_dead_case_26:
        movl    $6026, %eax
        retq

.Lmod140_dead_case_27:
        movl    $6027, %eax
        retq

.Lmod140_dead_case_28:
        movl    $6028, %eax
        retq

.Lmod140_dead_case_29:
        movl    $6029, %eax
        retq

.Lmod140_dead_case_30:
        movl    $6030, %eax
        retq

.Lmod140_dead_case_31:
        movl    $6031, %eax
        retq

.Lmod140_dead_case_32:
        movl    $6032, %eax
        retq

.Lmod140_dead_case_33:
        movl    $6033, %eax
        retq

.Lmod140_dead_case_34:
        movl    $6034, %eax
        retq

.Lmod140_dead_case_35:
        movl    $6035, %eax
        retq

.Lmod140_dead_case_36:
        movl    $6036, %eax
        retq

.Lmod140_dead_case_37:
        movl    $6037, %eax
        retq

.Lmod140_dead_case_38:
        movl    $6038, %eax
        retq

.Lmod140_dead_case_39:
        movl    $6039, %eax
        retq

.Lmod140_dead_case_40:
        movl    $6040, %eax
        retq

.Lmod140_dead_case_41:
        movl    $6041, %eax
        retq

.Lmod140_dead_case_42:
        movl    $6042, %eax
        retq

.Lmod140_dead_case_43:
        movl    $6043, %eax
        retq

.Lmod140_dead_case_44:
        movl    $6044, %eax
        retq

.Lmod140_dead_case_45:
        movl    $6045, %eax
        retq

.Lmod140_dead_case_46:
        movl    $6046, %eax
        retq

.Lmod140_dead_case_47:
        movl    $6047, %eax
        retq

.Lmod140_dead_case_48:
        movl    $6048, %eax
        retq

.Lmod140_dead_case_49:
        movl    $6049, %eax
        retq

.Lmod140_dead_case_50:
        movl    $6050, %eax
        retq

.Lmod140_dead_case_51:
        movl    $6051, %eax
        retq

.Lmod140_dead_case_52:
        movl    $6052, %eax
        retq

.Lmod140_dead_case_53:
        movl    $6053, %eax
        retq

.Lmod140_dead_case_54:
        movl    $6054, %eax
        retq

.Lmod140_dead_case_55:
        movl    $6055, %eax
        retq

.Lmod140_dead_case_56:
        movl    $6056, %eax
        retq

.Lmod140_dead_case_57:
        movl    $6057, %eax
        retq

.Lmod140_dead_case_58:
        movl    $6058, %eax
        retq

.Lmod140_dead_case_59:
        movl    $6059, %eax
        retq

.Lmod140_dead_case_60:
        movl    $6060, %eax
        retq

.Lmod140_dead_case_61:
        movl    $6061, %eax
        retq

.Lmod140_dead_case_62:
        movl    $6062, %eax
        retq

.Lmod140_dead_case_63:
        movl    $6063, %eax
        retq

.Lmod140_dead_case_64:
        movl    $6064, %eax
        retq

.Lmod140_dead_case_65:
        movl    $6065, %eax
        retq

.Lmod140_dead_case_66:
        movl    $6066, %eax
        retq

.Lmod140_dead_case_67:
        movl    $6067, %eax
        retq

.Lmod140_dead_case_68:
        movl    $6068, %eax
        retq

.Lmod140_dead_case_69:
        movl    $6069, %eax
        retq

.Lmod140_dead_case_70:
        movl    $6070, %eax
        retq

.Lmod140_dead_case_71:
        movl    $6071, %eax
        retq

.Lmod140_dead_case_72:
        movl    $6072, %eax
        retq

.Lmod140_dead_case_73:
        movl    $6073, %eax
        retq

.Lmod140_dead_case_74:
        movl    $6074, %eax
        retq

.Lmod140_dead_case_75:
        movl    $6075, %eax
        retq

.Lmod140_dead_case_76:
        movl    $6076, %eax
        retq

.Lmod140_dead_case_77:
        movl    $6077, %eax
        retq

.Lmod140_dead_case_78:
        movl    $6078, %eax
        retq

.Lmod140_dead_case_79:
        movl    $6079, %eax
        retq

.Lmod140_dead_case_80:
        movl    $6080, %eax
        retq

.Lmod140_dead_case_81:
        movl    $6081, %eax
        retq

.Lmod140_dead_case_82:
        movl    $6082, %eax
        retq

.Lmod140_dead_case_83:
        movl    $6083, %eax
        retq

.Lmod140_dead_case_84:
        movl    $6084, %eax
        retq

.Lmod140_dead_case_85:
        movl    $6085, %eax
        retq

.Lmod140_dead_case_86:
        movl    $6086, %eax
        retq

.Lmod140_dead_case_87:
        movl    $6087, %eax
        retq

.Lmod140_dead_case_88:
        movl    $6088, %eax
        retq

.Lmod140_dead_case_89:
        movl    $6089, %eax
        retq

.Lmod140_dead_case_90:
        movl    $6090, %eax
        retq

.Lmod140_dead_case_91:
        movl    $6091, %eax
        retq

.Lmod140_dead_case_92:
        movl    $6092, %eax
        retq

.Lmod140_dead_case_93:
        movl    $6093, %eax
        retq

.Lmod140_dead_case_94:
        movl    $6094, %eax
        retq

.Lmod140_dead_case_95:
        movl    $6095, %eax
        retq

.Lmod140_dead_case_96:
        movl    $6096, %eax
        retq

.Lmod140_dead_case_97:
        movl    $6097, %eax
        retq

.Lmod140_dead_case_98:
        movl    $6098, %eax
        retq

.Lmod140_dead_case_99:
        movl    $6099, %eax
        retq

.Lmod140_dead_case_100:
        movl    $6100, %eax
        retq

.Lmod140_dead_case_101:
        movl    $6101, %eax
        retq

.Lmod140_dead_case_102:
        movl    $6102, %eax
        retq

.Lmod140_dead_case_103:
        movl    $6103, %eax
        retq

.Lmod140_dead_case_104:
        movl    $6104, %eax
        retq

.Lmod140_dead_case_105:
        movl    $6105, %eax
        retq

.Lmod140_dead_case_106:
        movl    $6106, %eax
        retq

.Lmod140_dead_case_107:
        movl    $6107, %eax
        retq

.Lmod140_dead_case_108:
        movl    $6108, %eax
        retq

.Lmod140_dead_case_109:
        movl    $6109, %eax
        retq

.Lmod140_dead_case_110:
        movl    $6110, %eax
        retq

.Lmod140_dead_case_111:
        movl    $6111, %eax
        retq

.Lmod140_dead_case_112:
        movl    $6112, %eax
        retq

.Lmod140_dead_case_113:
        movl    $6113, %eax
        retq

.Lmod140_dead_case_114:
        movl    $6114, %eax
        retq

.Lmod140_dead_case_115:
        movl    $6115, %eax
        retq

.Lmod140_dead_case_116:
        movl    $6116, %eax
        retq

.Lmod140_dead_case_117:
        movl    $6117, %eax
        retq

.Lmod140_dead_case_118:
        movl    $6118, %eax
        retq

.Lmod140_dead_case_119:
        movl    $6119, %eax
        retq

.Lmod140_dead_case_120:
        movl    $6120, %eax
        retq

.Lmod140_dead_case_121:
        movl    $6121, %eax
        retq

.Lmod140_dead_case_122:
        movl    $6122, %eax
        retq

.Lmod140_dead_case_123:
        movl    $6123, %eax
        retq

.Lmod140_dead_case_124:
        movl    $6124, %eax
        retq

.Lmod140_dead_case_125:
        movl    $6125, %eax
        retq

.Lmod140_dead_case_126:
        movl    $6126, %eax
        retq

.Lmod140_dead_case_127:
        movl    $6127, %eax
        retq

.Lmod140_dead_case_128:
        movl    $6128, %eax
        retq

.Lmod140_dead_case_129:
        movl    $6129, %eax
        retq

.Lmod140_dead_case_130:
        movl    $6130, %eax
        retq

.Lmod140_dead_case_131:
        movl    $6131, %eax
        retq

.Lmod140_dead_case_132:
        movl    $6132, %eax
        retq

.Lmod140_dead_case_133:
        movl    $6133, %eax
        retq

.Lmod140_dead_case_134:
        movl    $6134, %eax
        retq

.Lmod140_dead_case_135:
        movl    $6135, %eax
        retq

.Lmod140_dead_case_136:
        movl    $6136, %eax
        retq

.Lmod140_dead_case_137:
        movl    $6137, %eax
        retq

.Lmod140_dead_case_138:
        movl    $6138, %eax
        retq

.Lmod140_dead_case_139:
        movl    $6139, %eax
        retq
        .size   jt_modulo_u140_dead_producer, .-jt_modulo_u140_dead_producer

        .section .rodata,"a",@progbits
        .p2align 2
.Lmod140_dead_table:
        .long   .Lmod140_dead_case_0-.Lmod140_dead_table
        .long   .Lmod140_dead_case_1-.Lmod140_dead_table
        .long   .Lmod140_dead_case_2-.Lmod140_dead_table
        .long   .Lmod140_dead_case_3-.Lmod140_dead_table
        .long   .Lmod140_dead_case_4-.Lmod140_dead_table
        .long   .Lmod140_dead_case_5-.Lmod140_dead_table
        .long   .Lmod140_dead_case_6-.Lmod140_dead_table
        .long   .Lmod140_dead_case_7-.Lmod140_dead_table
        .long   .Lmod140_dead_case_8-.Lmod140_dead_table
        .long   .Lmod140_dead_case_9-.Lmod140_dead_table
        .long   .Lmod140_dead_case_10-.Lmod140_dead_table
        .long   .Lmod140_dead_case_11-.Lmod140_dead_table
        .long   .Lmod140_dead_case_12-.Lmod140_dead_table
        .long   .Lmod140_dead_case_13-.Lmod140_dead_table
        .long   .Lmod140_dead_case_14-.Lmod140_dead_table
        .long   .Lmod140_dead_case_15-.Lmod140_dead_table
        .long   .Lmod140_dead_case_16-.Lmod140_dead_table
        .long   .Lmod140_dead_case_17-.Lmod140_dead_table
        .long   .Lmod140_dead_case_18-.Lmod140_dead_table
        .long   .Lmod140_dead_case_19-.Lmod140_dead_table
        .long   .Lmod140_dead_case_20-.Lmod140_dead_table
        .long   .Lmod140_dead_case_21-.Lmod140_dead_table
        .long   .Lmod140_dead_case_22-.Lmod140_dead_table
        .long   .Lmod140_dead_case_23-.Lmod140_dead_table
        .long   .Lmod140_dead_case_24-.Lmod140_dead_table
        .long   .Lmod140_dead_case_25-.Lmod140_dead_table
        .long   .Lmod140_dead_case_26-.Lmod140_dead_table
        .long   .Lmod140_dead_case_27-.Lmod140_dead_table
        .long   .Lmod140_dead_case_28-.Lmod140_dead_table
        .long   .Lmod140_dead_case_29-.Lmod140_dead_table
        .long   .Lmod140_dead_case_30-.Lmod140_dead_table
        .long   .Lmod140_dead_case_31-.Lmod140_dead_table
        .long   .Lmod140_dead_case_32-.Lmod140_dead_table
        .long   .Lmod140_dead_case_33-.Lmod140_dead_table
        .long   .Lmod140_dead_case_34-.Lmod140_dead_table
        .long   .Lmod140_dead_case_35-.Lmod140_dead_table
        .long   .Lmod140_dead_case_36-.Lmod140_dead_table
        .long   .Lmod140_dead_case_37-.Lmod140_dead_table
        .long   .Lmod140_dead_case_38-.Lmod140_dead_table
        .long   .Lmod140_dead_case_39-.Lmod140_dead_table
        .long   .Lmod140_dead_case_40-.Lmod140_dead_table
        .long   .Lmod140_dead_case_41-.Lmod140_dead_table
        .long   .Lmod140_dead_case_42-.Lmod140_dead_table
        .long   .Lmod140_dead_case_43-.Lmod140_dead_table
        .long   .Lmod140_dead_case_44-.Lmod140_dead_table
        .long   .Lmod140_dead_case_45-.Lmod140_dead_table
        .long   .Lmod140_dead_case_46-.Lmod140_dead_table
        .long   .Lmod140_dead_case_47-.Lmod140_dead_table
        .long   .Lmod140_dead_case_48-.Lmod140_dead_table
        .long   .Lmod140_dead_case_49-.Lmod140_dead_table
        .long   .Lmod140_dead_case_50-.Lmod140_dead_table
        .long   .Lmod140_dead_case_51-.Lmod140_dead_table
        .long   .Lmod140_dead_case_52-.Lmod140_dead_table
        .long   .Lmod140_dead_case_53-.Lmod140_dead_table
        .long   .Lmod140_dead_case_54-.Lmod140_dead_table
        .long   .Lmod140_dead_case_55-.Lmod140_dead_table
        .long   .Lmod140_dead_case_56-.Lmod140_dead_table
        .long   .Lmod140_dead_case_57-.Lmod140_dead_table
        .long   .Lmod140_dead_case_58-.Lmod140_dead_table
        .long   .Lmod140_dead_case_59-.Lmod140_dead_table
        .long   .Lmod140_dead_case_60-.Lmod140_dead_table
        .long   .Lmod140_dead_case_61-.Lmod140_dead_table
        .long   .Lmod140_dead_case_62-.Lmod140_dead_table
        .long   .Lmod140_dead_case_63-.Lmod140_dead_table
        .long   .Lmod140_dead_case_64-.Lmod140_dead_table
        .long   .Lmod140_dead_case_65-.Lmod140_dead_table
        .long   .Lmod140_dead_case_66-.Lmod140_dead_table
        .long   .Lmod140_dead_case_67-.Lmod140_dead_table
        .long   .Lmod140_dead_case_68-.Lmod140_dead_table
        .long   .Lmod140_dead_case_69-.Lmod140_dead_table
        .long   .Lmod140_dead_case_70-.Lmod140_dead_table
        .long   .Lmod140_dead_case_71-.Lmod140_dead_table
        .long   .Lmod140_dead_case_72-.Lmod140_dead_table
        .long   .Lmod140_dead_case_73-.Lmod140_dead_table
        .long   .Lmod140_dead_case_74-.Lmod140_dead_table
        .long   .Lmod140_dead_case_75-.Lmod140_dead_table
        .long   .Lmod140_dead_case_76-.Lmod140_dead_table
        .long   .Lmod140_dead_case_77-.Lmod140_dead_table
        .long   .Lmod140_dead_case_78-.Lmod140_dead_table
        .long   .Lmod140_dead_case_79-.Lmod140_dead_table
        .long   .Lmod140_dead_case_80-.Lmod140_dead_table
        .long   .Lmod140_dead_case_81-.Lmod140_dead_table
        .long   .Lmod140_dead_case_82-.Lmod140_dead_table
        .long   .Lmod140_dead_case_83-.Lmod140_dead_table
        .long   .Lmod140_dead_case_84-.Lmod140_dead_table
        .long   .Lmod140_dead_case_85-.Lmod140_dead_table
        .long   .Lmod140_dead_case_86-.Lmod140_dead_table
        .long   .Lmod140_dead_case_87-.Lmod140_dead_table
        .long   .Lmod140_dead_case_88-.Lmod140_dead_table
        .long   .Lmod140_dead_case_89-.Lmod140_dead_table
        .long   .Lmod140_dead_case_90-.Lmod140_dead_table
        .long   .Lmod140_dead_case_91-.Lmod140_dead_table
        .long   .Lmod140_dead_case_92-.Lmod140_dead_table
        .long   .Lmod140_dead_case_93-.Lmod140_dead_table
        .long   .Lmod140_dead_case_94-.Lmod140_dead_table
        .long   .Lmod140_dead_case_95-.Lmod140_dead_table
        .long   .Lmod140_dead_case_96-.Lmod140_dead_table
        .long   .Lmod140_dead_case_97-.Lmod140_dead_table
        .long   .Lmod140_dead_case_98-.Lmod140_dead_table
        .long   .Lmod140_dead_case_99-.Lmod140_dead_table
        .long   .Lmod140_dead_case_100-.Lmod140_dead_table
        .long   .Lmod140_dead_case_101-.Lmod140_dead_table
        .long   .Lmod140_dead_case_102-.Lmod140_dead_table
        .long   .Lmod140_dead_case_103-.Lmod140_dead_table
        .long   .Lmod140_dead_case_104-.Lmod140_dead_table
        .long   .Lmod140_dead_case_105-.Lmod140_dead_table
        .long   .Lmod140_dead_case_106-.Lmod140_dead_table
        .long   .Lmod140_dead_case_107-.Lmod140_dead_table
        .long   .Lmod140_dead_case_108-.Lmod140_dead_table
        .long   .Lmod140_dead_case_109-.Lmod140_dead_table
        .long   .Lmod140_dead_case_110-.Lmod140_dead_table
        .long   .Lmod140_dead_case_111-.Lmod140_dead_table
        .long   .Lmod140_dead_case_112-.Lmod140_dead_table
        .long   .Lmod140_dead_case_113-.Lmod140_dead_table
        .long   .Lmod140_dead_case_114-.Lmod140_dead_table
        .long   .Lmod140_dead_case_115-.Lmod140_dead_table
        .long   .Lmod140_dead_case_116-.Lmod140_dead_table
        .long   .Lmod140_dead_case_117-.Lmod140_dead_table
        .long   .Lmod140_dead_case_118-.Lmod140_dead_table
        .long   .Lmod140_dead_case_119-.Lmod140_dead_table
        .long   .Lmod140_dead_case_120-.Lmod140_dead_table
        .long   .Lmod140_dead_case_121-.Lmod140_dead_table
        .long   .Lmod140_dead_case_122-.Lmod140_dead_table
        .long   .Lmod140_dead_case_123-.Lmod140_dead_table
        .long   .Lmod140_dead_case_124-.Lmod140_dead_table
        .long   .Lmod140_dead_case_125-.Lmod140_dead_table
        .long   .Lmod140_dead_case_126-.Lmod140_dead_table
        .long   .Lmod140_dead_case_127-.Lmod140_dead_table
        .long   .Lmod140_dead_case_128-.Lmod140_dead_table
        .long   .Lmod140_dead_case_129-.Lmod140_dead_table
        .long   .Lmod140_dead_case_130-.Lmod140_dead_table
        .long   .Lmod140_dead_case_131-.Lmod140_dead_table
        .long   .Lmod140_dead_case_132-.Lmod140_dead_table
        .long   .Lmod140_dead_case_133-.Lmod140_dead_table
        .long   .Lmod140_dead_case_134-.Lmod140_dead_table
        .long   .Lmod140_dead_case_135-.Lmod140_dead_table
        .long   .Lmod140_dead_case_136-.Lmod140_dead_table
        .long   .Lmod140_dead_case_137-.Lmod140_dead_table
        .long   .Lmod140_dead_case_138-.Lmod140_dead_table
        .long   .Lmod140_dead_case_139-.Lmod140_dead_table
        .long   0

        .section .note.GNU-stack,"",@progbits
