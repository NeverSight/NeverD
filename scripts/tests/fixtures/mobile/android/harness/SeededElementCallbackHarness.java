import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;

public final class SeededElementCallbackHarness {
    static final RuntimeException FAILURE=new RuntimeException("owned seeded element callback failure");
    static final Error ERROR=new Error("owned seeded element callback error");
    static Object expectedSeed,firstResult,secondResult;
    static Scope expectedScope;
    static int calls,mode;
    static class Token {}
    static void require(boolean value) { if(!value)throw new AssertionError("owned seeded element callback observer"); }
    static final class Action implements Fn<Object,Context,Object> {
        public Object apply(Object input,Context context) {
            require(context==expectedScope && input==(calls==0?expectedSeed:firstResult));
            int number=++calls;Trace.hit(number==1?2:4);
            if(mode==15)throw ERROR;
            return number==1?firstResult:secondResult;
        }
    }
    static Object call(int route,Scope scope,Object seed,Fn fn) throws Throwable {
        if(route==0)return Client.run(scope,seed,fn);
        if(route==1)return SeededElementCallbackCaller.run(scope,seed,fn);
        if(route==2)try {return Scope.Helper.class.getMethod("fold",Scope.class,Object.class,Fn.class).invoke(null,scope,seed,fn);}
        catch(InvocationTargetException e){throw e.getCause();}
        MethodHandle handle=MethodHandles.publicLookup().findStatic(Scope.Helper.class,"fold",MethodType.methodType(Object.class,Scope.class,Object.class,Fn.class));
        return (Object)handle.invokeExact(scope,seed,fn);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]);mode=Integer.parseInt(args[1]);
        Method method=Scope.Helper.class.getMethod("fold",Scope.class,Object.class,Fn.class);
        require(Scope.Helper.class.getEnclosingClass()==Scope.class && Modifier.isStatic(Scope.Helper.class.getModifiers()));
        TypeVariable<?> formal=method.getTypeParameters()[0];
        ParameterizedType fn=(ParameterizedType)method.getGenericParameterTypes()[2];
        require(formal.getBounds()[0]==Object.class && method.getGenericReturnType().equals(formal));
        require(((WildcardType)fn.getActualTypeArguments()[0]).getLowerBounds()[0].equals(formal));
        require(((WildcardType)fn.getActualTypeArguments()[1]).getLowerBounds()[0]==Context.class);
        require(((WildcardType)fn.getActualTypeArguments()[2]).getUpperBounds()[0].equals(formal));
        for(int attempt=0;attempt<2;attempt++) {
            expectedScope=mode==6?null:new Scope();expectedSeed=mode==7?null:new Token();
            firstResult=mode==9?null:mode==14?new Object():new Token();
            secondResult=mode==10?null:mode==13?new Object():new Token();
            Object sentinel=new Token();Trace.result=mode==12?null:sentinel;Trace.override=mode==11||mode==12;
            Trace.seenContext=Trace.seenSeed=Trace.seenFn=Trace.first=Trace.second=null;
            Trace.trace=0;calls=0;Trace.failAt=mode>=1&&mode<=5?mode:0;Trace.failure=FAILURE;
            Fn fnValue=mode==8?null:new Action();Object result=null;Throwable failure=null;
            try {result=call(route,expectedScope,expectedSeed,fnValue);Token checked=(Token)result;require(checked==result);}
            catch(Throwable caught){failure=caught;}
            require(Trace.seenContext==expectedScope&&Trace.seenSeed==expectedSeed&&Trace.seenFn==fnValue);
            long expectedTrace=mode==1||mode==8?1:mode==2||mode==15?12:mode==3?123:mode==4?1234:12345;
            require(Trace.trace==expectedTrace);
            require(calls==(mode==1||mode==8?0:mode==2||mode==3||mode==15?1:2));
            require(Trace.first==(mode==1||mode==2||mode==8||mode==15?null:firstResult));
            require(Trace.second==(expectedTrace==12345?secondResult:null));
            if(mode>=1&&mode<=5)require(failure==FAILURE);
            else if(mode==8)require(failure instanceof NullPointerException);
            else if(mode==13)require(failure instanceof ClassCastException&&result==secondResult);
            else if(mode==15)require(failure==ERROR);
            else require(failure==null&&result==(Trace.override?Trace.result:secondResult));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.trace);
        }
    }
}
