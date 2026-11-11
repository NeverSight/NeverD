package fixture;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;

public final class EmptyPackageMetadataHarness {
    private static void check(boolean condition) {
        if(!condition)throw new AssertionError("owned empty package metadata mismatch");
    }
    private static void metadata(String name,Class anchor) {
        try {
            Class type=Class.forName(name);
            check(type.getName().equals(name) && type.getSimpleName().equals("package-info"));
            check(type.isInterface() && type.isSynthetic() && !type.isAnnotation());
            check(type.getModifiers()==0x1600 && type.getSuperclass()==null);
            check(type.getInterfaces().length==0 && type.getTypeParameters().length==0);
            check(type.getDeclaredMethods().length==0 && type.getDeclaredFields().length==0);
            check(type.getDeclaredConstructors().length==0 && type.getDeclaredAnnotations().length==0);
            check(type.getEnclosingClass()==null && type.getDeclaringClass()==null);
            check(type.getPackage()==anchor.getPackage() && type.getPackage().getDeclaredAnnotations().length==0);
        }catch(Throwable failure){throw new AssertionError("owned empty package metadata mismatch",failure);}
    }
    public static void main(String[] args)throws Throwable {
        int route=Integer.parseInt(args[0]),value=Integer.parseInt(args[1]);
        check(route>=0 && route<3);
        for(int attempt=0;attempt<2;++attempt) {
            metadata("fixture.alpha.package-info",fixture.alpha.Anchor.class);
            metadata("fixture.beta.package-info",fixture.beta.Anchor.class);
            fixture.alpha.Anchor.calls=0;fixture.beta.Anchor.calls=0;
            Object result=null;
            if(route==0)result=Core.run(value);
            else if(route==1)result=MethodHandles.lookup().findStatic(Core.class,"run",
                MethodType.methodType(int.class,int.class)).invokeWithArguments(value);
            else Core.ignore(value);
            check(fixture.alpha.Anchor.calls==1 && fixture.beta.Anchor.calls==1);
            check(route==2 ? result==null : ((Integer)result).intValue()==(value+3)*5);
            metadata("fixture.alpha.package-info",fixture.alpha.Anchor.class);
            metadata("fixture.beta.package-info",fixture.beta.Anchor.class);
        }
        System.out.println("attempts=2,empty-package-metadata=matched");
    }
}
