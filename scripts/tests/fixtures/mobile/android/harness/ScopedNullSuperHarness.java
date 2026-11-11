import fixture.Child;
import fixture.Parent;
import fixture.Trace;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
import java.util.function.Function;
public final class ScopedNullSuperHarness {
    private static void require(boolean value) {if(!value)throw new AssertionError("owned scoped null super");}
    @SuppressWarnings({"rawtypes","unchecked"})
    private static Child<?> call(int route,int kind,boolean flag,boolean secondary,Function fn) throws Throwable {
        if(route==0)return kind==0?new Child(flag,fn):new Child(flag,secondary,fn);
        if(route==1)return kind==0?ScopedNullSuperCaller.defaults(flag,fn):ScopedNullSuperCaller.explicit(flag,secondary,fn);
        if(route==2) {
            try {return (Child<?>)(kind==0?Child.class.getConstructor(boolean.class,Function.class).newInstance(flag,fn):Child.class.getConstructor(boolean.class,boolean.class,Function.class).newInstance(flag,secondary,fn));}
            catch(InvocationTargetException failure) {throw failure.getCause();}
        }
        if(kind==0)return (Child)MethodHandles.publicLookup().findConstructor(Child.class,MethodType.methodType(void.class,boolean.class,Function.class)).invokeExact(flag,fn);
        return (Child)MethodHandles.publicLookup().findConstructor(Child.class,MethodType.methodType(void.class,boolean.class,boolean.class,Function.class)).invokeExact(flag,secondary,fn);
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    public static void main(String[] args) throws Throwable {
        final int route=Integer.parseInt(args[0]),kind=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final boolean flag=kind!=1,secondary=kind==2;
        final Object marker=new Object(),other=new Object(),stored=new Object(),computed=new Object();
        final RuntimeException parentFailure=new RuntimeException("owned parent failure"),functionFailure=new RuntimeException("owned function failure"),compareFailure=new RuntimeException("owned compare failure");
        final Error parentError=new Error("owned parent error");
        final Object desired=mode==8?new Object():Integer.valueOf(Integer.MIN_VALUE+37+mode);
        final Comparable<Object> comparable=new Comparable<Object>() {public int compareTo(Object value) {
            require(value==other&&Trace.payload==computed&&Trace.events==132456);Trace.right=value;Trace.hit(7);Trace.compareAction.run();return ((Integer)Trace.comparison).intValue();
        }};
        final Function<Object,Comparable<?>> observed=new Function<Object,Comparable<?>>() {public Comparable<?> apply(Object value) {
            require(value==marker&&Trace.payload==stored&&Trace.events==1324);Trace.left=value;Trace.hit(5);Trace.functionAction.run();return (Comparable<?>)Trace.result;
        }};
        final Function<String,Comparable<?>> fn=mode==11?null:(Function)observed;
        Trace.failure=new RuntimeException("owned stage failure");Trace.failAt=mode==1?1:mode==4?2:mode==5?4:0;
        Object previousPartial=null;
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object(),oldPartial=previousPartial;
            Trace.events=0;Trace.partial=Trace.payload=Trace.result=Trace.comparison=Trace.left=Trace.right=sentinel;int parents=Trace.parents;
            Trace.action=new Runnable(){public void run(){
                require(Trace.events==1&&Trace.partial instanceof Child&&Trace.partial!=oldPartial);
                Child<?> partial=(Child<?>)Trace.partial;require(partial.flag==flag&&partial.cmp==null&&!partial.secondary&&partial.fn==null);
                Trace.hit(3);Trace.payload=stored;if(mode==2)throw parentFailure;if(mode==3)throw parentError;
            }};
            Trace.functionAction=new Runnable(){public void run(){
                require(Trace.events==13245&&Trace.left==marker&&Trace.right==sentinel);Trace.hit(6);Trace.payload=computed;
                Trace.result=mode==9?new Object():mode==10?null:comparable;if(mode==6)throw functionFailure;
            }};
            Trace.compareAction=new Runnable(){public void run(){
                require(Trace.events==1324567&&Trace.left==marker&&Trace.right==other);Trace.hit(8);Trace.comparison=desired;if(mode==7)throw compareFailure;
            }};
            Child<?> actual=null;Throwable failure=null;int answer=42;
            try {actual=call(route,kind,flag,secondary,fn);Comparable value=(Comparable)((Function)actual.fn).apply(marker);answer=value.compareTo(other);} catch(Throwable error){failure=error;}
            long events=mode==1?1:mode==2||mode==3?13:mode==4?132:mode==5||mode==11?1324:mode==6||mode==9||mode==10?132456:13245678;
            require(Trace.events==events&&Trace.parents==parents+1);
            Throwable expected=mode==1||mode==4||mode==5?Trace.failure:mode==2?parentFailure:mode==3?parentError:mode==6?functionFailure:mode==7?compareFailure:null;
            boolean constructorFailure=mode>=1&&mode<=5;
            if(expected!=null)require(failure==expected&&actual==(constructorFailure?null:Trace.partial)&&answer==42);
            else if(mode==8||mode==9)require(failure instanceof ClassCastException&&actual==Trace.partial&&answer==42);
            else if(mode==10||mode==11)require(failure instanceof NullPointerException&&actual==Trace.partial&&answer==42);
            else require(failure==null&&actual==Trace.partial&&answer==((Integer)desired).intValue());
            if(mode==1)require(Trace.partial==sentinel);
            else {Child<?> partial=(Child<?>)Trace.partial;require(partial!=previousPartial&&partial.flag==flag&&partial.cmp==null);
                require(partial.secondary==(mode>=2&&mode<=4?false:secondary)&&(Object)partial.fn==(mode>=2&&mode<=4?null:fn));previousPartial=partial;}
            System.out.println("route="+route+",operation="+kind+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
