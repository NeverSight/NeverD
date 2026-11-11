import java.nio.file.*;
import jdk.internal.org.objectweb.asm.*;
import static jdk.internal.org.objectweb.asm.Opcodes.*;
public final class IntegerConstructorChoiceInputBuilder {
    public static void main(String[] args) throws Exception {
        ClassWriter writer=new ClassWriter(ClassWriter.COMPUTE_FRAMES|ClassWriter.COMPUTE_MAXS);
        writer.visit(V1_8,ACC_PUBLIC|ACC_FINAL|ACC_SUPER,"fixture/Core",null,"java/lang/Object",null);
        writer.visitSource("Core.java",null);
        MethodVisitor ctor=writer.visitMethod(ACC_PRIVATE,"<init>","()V",null,null);
        ctor.visitCode();ctor.visitVarInsn(ALOAD,0);ctor.visitMethodInsn(INVOKESPECIAL,"java/lang/Object","<init>","()V",false);ctor.visitInsn(RETURN);ctor.visitMaxs(0,0);ctor.visitEnd();
        MethodVisitor m=writer.visitMethod(ACC_PUBLIC|ACC_STATIC,"make","(I)Lfixture/Widget;",null,null);
        Label start=new Label(),copy=new Label(),join=new Label(),end=new Label(),handler=new Label();
        m.visitTryCatchBlock(start,end,handler,"java/lang/Exception");m.visitCode();m.visitLabel(start);
        m.visitTypeInsn(NEW,"fixture/Widget");m.visitInsn(DUP);m.visitVarInsn(ILOAD,0);
        m.visitJumpInsn(Boolean.parseBoolean(args[1])?IFLE:IFGT,copy);
        m.visitMethodInsn(INVOKESPECIAL,"fixture/Widget","<init>","()V",false);m.visitJumpInsn(GOTO,join);
        m.visitLabel(copy);m.visitVarInsn(ILOAD,0);
        m.visitMethodInsn(INVOKESPECIAL,"fixture/Widget","<init>","(I)V",false);
        m.visitLabel(join);m.visitInsn(ARETURN);m.visitLabel(end);
        m.visitLabel(handler);m.visitVarInsn(ASTORE,1);m.visitVarInsn(ALOAD,1);
        m.visitFieldInsn(PUTSTATIC,"fixture/Trace","caught","Ljava/lang/Throwable;");
        m.visitIntInsn(BIPUSH,8);m.visitMethodInsn(INVOKESTATIC,"fixture/Trace","hit","(I)V",false);
        m.visitInsn(ACONST_NULL);m.visitInsn(ARETURN);m.visitMaxs(0,0);m.visitEnd();writer.visitEnd();
        Path output=Paths.get(args[0]).resolve("fixture/Core.class");Files.createDirectories(output.getParent());
        Files.write(output,writer.toByteArray(),StandardOpenOption.CREATE_NEW);
    }
}
