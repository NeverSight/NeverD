import fixture.*;
import kotlin.Unit;
import kotlin.jvm.functions.Function1;
import java.lang.reflect.*;
public final class PublicFinalGenericNamesHarness {
    static int count,beforeCalls,actionCalls,afterCalls,observedBits,mode,attempt;
    static final int[] events=new int[8];
    static Object receiver,seenSeed;
    static Throwable failure;
    static void require(boolean condition){if(!condition)throw new AssertionError("owned public final generic names");}
    static void event(int value){require(count<events.length);events[count++]=value;}
    @SuppressWarnings("unchecked") static <E extends Throwable> void raise(Throwable error)throws E{throw (E)error;}
    static class First {public int hashCode(){throw new AssertionError("unexpected seed hash");}public String toString(){throw new AssertionError("unexpected seed text");}}
    static final class Second extends First {}
    static final class Wrong {public int hashCode(){throw new AssertionError("unexpected wrong hash");}}
    static final class Recording extends Child {
        Recording(Object seed){super(seed);}
        public void before(int bits){event(1);beforeCalls++;receiver=this;observedBits=bits;if(mode==5)PublicFinalGenericNamesHarness.<RuntimeException>raise(failure);}
        public void after(){event(3);afterCalls++;if(mode==7)PublicFinalGenericNamesHarness.<RuntimeException>raise(failure);}
        public int hashCode(){throw new AssertionError("unexpected receiver hash");}
    }
    static Unit action(Object value){event(2);actionCalls++;seenSeed=value;if(mode==6)PublicFinalGenericNamesHarness.<RuntimeException>raise(failure);if(mode==9)throw (RuntimeException)null;return Unit.INSTANCE;}
    static final class Raw implements Function1<Object,Unit>{public Unit invoke(Object value){return action(value);}public int hashCode(){throw new AssertionError("unexpected callback hash");}}
    static final class Typed implements Function1<First,Unit>{public Unit invoke(First value){return action(value);}public int hashCode(){throw new AssertionError("unexpected typed callback hash");}}
    static Method metadata(String name)throws Exception{
        Method m=Root.class.getDeclaredMethod(name,int.class,Function1.class);int flags=m.getModifiers();require(Modifier.isPublic(flags)&&Modifier.isFinal(flags)&&!Modifier.isStatic(flags)&&!Modifier.isAbstract(flags)&&!m.isBridge());
        require(m.isSynthetic()==name.equals("walk-stage"));require(Modifier.isAbstract(Root.class.getModifiers())&&!Modifier.isFinal(Root.class.getModifiers()));
        TypeVariable<Method>[] variables=m.getTypeParameters();require(variables.length==1&&variables[0].getGenericDeclaration().equals(m)&&variables[0].getBounds().length==1&&variables[0].getBounds()[0]==Object.class);
        Type[] parameters=m.getGenericParameterTypes();require(parameters.length==2&&parameters[0]==int.class&&m.getGenericReturnType()==void.class);
        ParameterizedType callback=(ParameterizedType)parameters[1];require(callback.getRawType()==Function1.class);Type[] args=callback.getActualTypeArguments();require(args.length==2&&args[1]==Unit.class);
        WildcardType input=(WildcardType)args[0];require(input.getLowerBounds().length==1&&input.getLowerBounds()[0].equals(variables[0])&&input.getUpperBounds().length==1&&input.getUpperBounds()[0]==Object.class);
        require(Child.class.getMethod(name,int.class,Function1.class).equals(m));m.setAccessible(true);return m;
    }
    @SuppressWarnings({"unchecked","rawtypes"}) public static void main(String[] args)throws Throwable{
        int route=Integer.parseInt(args[0]),kind=Integer.parseInt(args[1]);mode=Integer.parseInt(args[2]);Method method=metadata(args[3]);Throwable previous=null;
        for(attempt=0;attempt<2;attempt++){
            count=beforeCalls=actionCalls=afterCalls=observedBits=0;receiver=seenSeed=null;
            failure=attempt==0?(mode==7?new AssertionError("owned late error"):new IllegalArgumentException("owned exact runtime")):new Exception("owned exact checked");
            Object seed=mode==2?null:mode==3?new Wrong():attempt==0?new First():new Second();Recording value=new Recording(seed),other=new Recording(new First());Recording target=mode==8?null:value;
            Function1 callback=mode==4?null:kind==0?new Raw():new Typed();int bits=mode==1?0:attempt==0?Integer.MIN_VALUE:Integer.MAX_VALUE;Throwable error=null;
            try{if(route==0)CoreKt.sendRoot(target,bits,callback);else if(route==1)CoreKt.sendChild(target,bits,callback);else try{(route==2?method:Child.class.getMethod(args[3],int.class,Function1.class)).invoke(target,bits,callback);}catch(InvocationTargetException e){throw e.getCause();}}catch(Throwable e){error=e;}
            require(other.seed!=seed&&value.seed==seed);
            if(mode==8){require(error!=null&&error.getClass()==NullPointerException.class&&error!=previous);require(count==0&&receiver==null&&seenSeed==null);}
            else if(mode==1){require(error==null&&count==0&&receiver==null&&seenSeed==null);}
            else{
                boolean beforeFailure=mode==5,bridgeFailure=kind==1&&mode==3,callbackFailure=mode==6||mode==9;
                require(beforeCalls==1&&receiver==value&&observedBits==bits&&events[0]==1);
                boolean called=!beforeFailure&&!bridgeFailure&&mode!=4;require(actionCalls==(called?1:0));require(!called?seenSeed==null:seenSeed==seed);if(called)require(events[1]==2);
                boolean after=!beforeFailure&&!bridgeFailure&&mode!=4&&!callbackFailure;require(afterCalls==(after?1:0));if(after)require(events[2]==3);require(count==1+(called?1:0)+(after?1:0));
                if(beforeFailure||mode==6||mode==7)require(error==failure);else if(bridgeFailure)require(error!=null&&error.getClass()==ClassCastException.class);else if(mode==4||mode==9)require(error!=null&&error.getClass()==NullPointerException.class&&error!=previous);else require(error==null);
            }
            previous=error;
        }
        System.out.println("attempts=2,public-final-generic-names=matched");
    }
}
