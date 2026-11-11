import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class GenericTerminalVoidCaller {
    public static Method method(String name,Class result) {
        Method found=null;
        for(Method m:Core.class.getDeclaredMethods())if(m.getName().equals(name)&&m.getReturnType()==result&&java.util.Arrays.equals(m.getParameterTypes(),new Class[]{Key.class,Object.class})) {
            if(found!=null)throw new AssertionError("owned generic terminal void");found=m;
        }
        if(found==null)throw new AssertionError("owned generic terminal void");return found;
    }
    public static void call(int route,int entry,Object key,Object value,String target)throws Throwable {
        if(route==3&&entry==0){((Base)Core.INSTANCE).writeValue((Key)key,value);return;}
        String name=entry==0?"writeValue":target;Class result=entry==0?void.class:Void.class;Method method=method(name,result);
        if(route==0){try{method.invoke(Core.INSTANCE,key,value);return;}catch(InvocationTargetException e){throw e.getCause();}}
        MethodHandle handle=route==3?MethodHandles.publicLookup().unreflect(method):MethodHandles.publicLookup().findVirtual(Core.class,name,MethodType.methodType(result,Key.class,Object.class));
        if(route==1||route==3){handle.invokeWithArguments(Core.INSTANCE,key,value);return;}
        if(entry==0)handle.invokeExact(Core.INSTANCE,(Key)key,value);
        else {Void ignored=(Void)handle.invokeExact(Core.INSTANCE,(Key)key,value);}
    }
}
