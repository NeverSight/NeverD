import java.nio.file.*;
import jdk.internal.org.objectweb.asm.*;
public final class ForwardAllocationInputBuilder implements Opcodes {
    public static void main(String[] args) throws Exception {
        ClassWriter writer = new ClassWriter(ClassWriter.COMPUTE_FRAMES | ClassWriter.COMPUTE_MAXS);
        writer.visit(V1_8, ACC_PUBLIC | ACC_FINAL | ACC_SUPER, "fixture/Client", null, "java/lang/Object", null);
        writer.visitSource("Client.java", null);
        MethodVisitor init = writer.visitMethod(ACC_PUBLIC, "<init>", "()V", null, null);
        init.visitCode();
        init.visitVarInsn(ALOAD, 0);
        init.visitMethodInsn(INVOKESPECIAL, "java/lang/Object", "<init>", "()V", false);
        init.visitInsn(RETURN); init.visitMaxs(0, 0); init.visitEnd();
        MethodVisitor method = writer.visitMethod(ACC_PUBLIC | ACC_STATIC, "make", "(I)Lfixture/Target;", null, null);
        Label start = new Label(), end = new Label(), handler = new Label();
        Label zero = new Label(), one = new Label(), other = new Label(), initialize = new Label();
        method.visitTryCatchBlock(start, end, handler, "java/lang/RuntimeException");
        method.visitCode(); method.visitLabel(start);
        method.visitInsn(ICONST_5);
        method.visitMethodInsn(INVOKESTATIC, "fixture/Trace", "hit", "(I)V", false);
        method.visitVarInsn(ILOAD, 0);
        method.visitTableSwitchInsn(0, 1, other, zero, one);
        method.visitLabel(other);
        method.visitTypeInsn(NEW, "fixture/Target"); method.visitInsn(DUP);
        method.visitJumpInsn(GOTO, initialize);
        method.visitLabel(zero);
        method.visitTypeInsn(NEW, "java/lang/IllegalStateException"); method.visitInsn(DUP);
        method.visitMethodInsn(INVOKESTATIC, "fixture/Trace", "message", "()Ljava/lang/String;", false);
        method.visitMethodInsn(INVOKESPECIAL, "java/lang/IllegalStateException", "<init>", "(Ljava/lang/String;)V", false);
        method.visitInsn(ATHROW);
        method.visitLabel(one); method.visitInsn(ACONST_NULL); method.visitInsn(ARETURN);
        method.visitLabel(initialize);
        method.visitMethodInsn(INVOKESPECIAL, "fixture/Target", "<init>", "()V", false);
        method.visitInsn(ARETURN);
        method.visitLabel(end); method.visitLabel(handler);
        method.visitVarInsn(ASTORE, 1); method.visitVarInsn(ALOAD, 1);
        method.visitFieldInsn(PUTSTATIC, "fixture/Trace", "caught", "Ljava/lang/RuntimeException;");
        method.visitInsn(ICONST_3);
        method.visitMethodInsn(INVOKESTATIC, "fixture/Trace", "hit", "(I)V", false);
        method.visitInsn(ACONST_NULL); method.visitInsn(ARETURN);
        method.visitMaxs(0, 0); method.visitEnd(); writer.visitEnd();
        Path path = Paths.get(args[0], "fixture/Client.class");
        Files.createDirectories(path.getParent());
        Files.write(path, writer.toByteArray(), StandardOpenOption.CREATE_NEW);
    }
}
