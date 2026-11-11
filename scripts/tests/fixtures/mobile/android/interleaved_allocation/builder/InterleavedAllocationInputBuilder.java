import java.nio.file.*;
import jdk.internal.org.objectweb.asm.*;
import static jdk.internal.org.objectweb.asm.Opcodes.*;
public final class InterleavedAllocationInputBuilder {
    static void field(MethodVisitor m,String name,String type){m.visitVarInsn(ALOAD,0);m.visitFieldInsn(GETFIELD,"fixture/Provider",name,type);}
    static void tail(MethodVisitor m){m.visitVarInsn(ALOAD,0);m.visitVarInsn(LLOAD,1);m.visitMethodInsn(INVOKEVIRTUAL,"fixture/Provider","finish","(J)J",false);}
    public static void main(String[] args) throws Exception {
        ClassWriter w=new ClassWriter(ClassWriter.COMPUTE_FRAMES|ClassWriter.COMPUTE_MAXS);
        w.visit(V1_8,ACC_PUBLIC|ACC_FINAL|ACC_SUPER,"fixture/Core",null,"java/lang/Object",null);w.visitSource("Core.java",null);
        MethodVisitor c=w.visitMethod(ACC_PRIVATE,"<init>","()V",null,null);
        c.visitCode();c.visitVarInsn(ALOAD,0);c.visitMethodInsn(INVOKESPECIAL,"java/lang/Object","<init>","()V",false);c.visitInsn(RETURN);c.visitMaxs(0,0);c.visitEnd();
        MethodVisitor m=w.visitMethod(ACC_PUBLIC|ACC_STATIC,"make","(Lfixture/Provider;J)Lfixture/Target;",null,null);
        boolean nested=Boolean.parseBoolean(args[1]);
        Label main=new Label(),other=new Label(),nil=new Label(),right=new Label(),init=new Label(),parent=new Label();
        m.visitCode();field(m,"branch","I");m.visitLookupSwitchInsn(main,new int[]{0,1},new Label[]{nil,nested?main:other});
        m.visitLabel(main);
        if(nested){m.visitTypeInsn(NEW,"fixture/Envelope");m.visitVarInsn(ASTORE,5);field(m,"branch","I");m.visitInsn(ICONST_1);m.visitJumpInsn(IF_ICMPEQ,other);}
        m.visitTypeInsn(NEW,"fixture/Target");m.visitVarInsn(ASTORE,3);
        field(m,"flag","I");m.visitJumpInsn(IFNE,right);field(m,"first","Ljava/lang/Object;");m.visitVarInsn(ASTORE,4);m.visitJumpInsn(GOTO,init);
        m.visitLabel(other);m.visitTypeInsn(NEW,"fixture/Target");m.visitInsn(DUP);field(m,"alternate","Ljava/lang/Object;");tail(m);
        m.visitMethodInsn(INVOKESPECIAL,"fixture/Target","<init>","(Ljava/lang/Object;J)V",false);
        if(nested){m.visitVarInsn(ASTORE,3);m.visitJumpInsn(GOTO,parent);}else m.visitInsn(ARETURN);
        m.visitLabel(nil);m.visitInsn(ACONST_NULL);m.visitInsn(ARETURN);
        m.visitLabel(right);field(m,"alternate","Ljava/lang/Object;");m.visitVarInsn(ASTORE,4);
        m.visitLabel(init);m.visitVarInsn(ALOAD,3);m.visitVarInsn(ALOAD,4);tail(m);
        m.visitMethodInsn(INVOKESPECIAL,"fixture/Target","<init>","(Ljava/lang/Object;J)V",false);
        if(nested){m.visitLabel(parent);m.visitVarInsn(ALOAD,5);m.visitVarInsn(ALOAD,3);m.visitMethodInsn(INVOKESPECIAL,"fixture/Envelope","<init>","(Lfixture/Target;)V",false);m.visitVarInsn(ALOAD,5);m.visitFieldInsn(GETFIELD,"fixture/Envelope","child","Lfixture/Target;");}
        else m.visitVarInsn(ALOAD,3);
        m.visitInsn(ARETURN);
        m.visitMaxs(0,0);m.visitEnd();w.visitEnd();
        Path p=Paths.get(args[0]).resolve("fixture/Core.class");Files.createDirectories(p.getParent());Files.write(p,w.toByteArray(),StandardOpenOption.CREATE_NEW);
    }
}
