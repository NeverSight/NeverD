import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public class FixedCompletionFactoryCaller {
    public static Completion call(int route,int kind,Object block,Object receiver,Completion completion)throws Throwable {
        if(route==3)return kind==0?Core.unary((Unary)block,completion):Core.seeded((Binary)block,receiver,completion);
        if(route==0) {
            try {
                if(kind==0)return (Completion)Core.class.getMethod("unary",Unary.class,Completion.class).invoke(null,block,completion);
                return (Completion)Core.class.getMethod("seeded",Binary.class,Object.class,Completion.class).invoke(null,block,receiver,completion);
            }catch(InvocationTargetException error){throw error.getCause();}
        }
        MethodType type=kind==0?MethodType.methodType(Completion.class,Unary.class,Completion.class):MethodType.methodType(Completion.class,Binary.class,Object.class,Completion.class);
        MethodHandle handle=MethodHandles.publicLookup().findStatic(Core.class,kind==0?"unary":"seeded",type);
        if(route==1)return (Completion)(kind==0?handle.invokeWithArguments(block,completion):handle.invokeWithArguments(block,receiver,completion));
        if(kind==0)return (Completion)handle.invokeExact((Unary)block,completion);
        return (Completion)handle.invokeExact((Binary)block,receiver,completion);
    }
}
