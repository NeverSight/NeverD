import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class GuardedTypedDefaultsHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned guarded typed defaults"); }
    static final class Callback implements Fn {
        public Object apply(Object input) { Trace.seenInput=input;Trace.hit(8);return Trace.reply; }
    }
    static final class Observation implements Observer {
        final Object token; final Fn callback; final Object[] array; final boolean mutate;
        Observation(Object token,Fn callback,Object[] array,boolean mutate) { this.token=token;this.callback=callback;this.array=array;this.mutate=mutate; }
        public void prepared(Child child) {
            require(child.bits==Trace.bits && child.token==token && child.marker==null);
            require(child.zeroCallback==null && child.zeroBits==0 && !child.zeroFlag && child.zeroArray==null);
            try {
                String[] names={"zeroCallback","zeroBits","zeroFlag","zeroArray"};
                Object[] values={callback,Long.valueOf(Long.MIN_VALUE+17),Boolean.TRUE,array};
                for(int i=0;i<names.length;i++) {
                    Field field=Child.class.getDeclaredField(names[i]);field.setAccessible(true);
                    require(Modifier.isFinal(field.getModifiers()) && !Modifier.isStatic(field.getModifiers()));
                    if(mutate)field.set(child,values[i]);
                }
            } catch(ReflectiveOperationException e) { throw new AssertionError(e); }
            Trace.hit(7);
        }
    }
    static Child build(int route,Object token) throws Throwable {
        try {
            if(route==0)return Client.build(token);
            if(route==1)return GuardedTypedDefaultsCaller.build(token);
            if(route==2)return Child.class.getConstructor(Object.class).newInstance(token);
            return (Child)MethodHandles.publicLookup().findConstructor(Child.class,
                    MethodType.methodType(void.class,Object.class)).invokeExact(token);
        } catch(InvocationTargetException e) { throw e.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),variant=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        for(int attempt=0;attempt<2;attempt++) {
            Trace.events=0;Trace.fail=mode>=1 && mode<=8 ? mode : 0;
            Trace.failure=new IllegalArgumentException("owned sentinel");Trace.partial=null;Trace.seenInput=null;
            Trace.bits=attempt==0 ? Long.MIN_VALUE : Long.MAX_VALUE;
            Trace.reply=mode==10 ? new Object() : mode==11 ? null : new Unit();
            Object token=variant==0 ? new Object() : null;
            Trace.fallback=mode==12 ? null : new Object();
            Object selected=token!=null ? token : Trace.fallback,input=mode==12 ? null : new Object();
            Callback callback=new Callback();Object[] array={new Object()};boolean mutate=mode!=20;
            Trace.observer=new Observation(selected,callback,array,mutate);
            int[] points=mutate ? new int[]{1,2,3,7,4,5,6,8} : new int[]{1,2,3,7,4,5,6};
            long expected=0;int last=0;boolean failed=false;
            for(int point:points) { expected=expected*31+point;last=point;if(point==Trace.fail) { failed=true;break; } }
            Child child=null;Object result=null;Throwable caught=null;
            try {
                child=build(route,token);
                result=route==0 ? Client.read(child,input) : GuardedTypedDefaultsCaller.read(child,input);
            } catch(Throwable e) { caught=e; }
            require(Trace.events==expected);
            if(failed)require(caught==Trace.failure);
            else if(mode==10)require(caught instanceof ClassCastException);
            else if(mode==20)require(caught instanceof NullPointerException);
            else require(caught==null && result==Trace.reply);
            boolean partial=last!=1 && last!=2;
            require((Trace.partial!=null)==partial);
            boolean returned=!failed || last==6 || last==8;
            require((child!=null)==returned);
            if(partial) {
                Child value=Trace.partial;boolean changed=mutate && last!=3;
                require(value.bits==Trace.bits && value.token==selected && value.marker==(returned ? selected : null));
                require(value.zeroCallback==(changed ? callback : null) && value.zeroBits==(changed ? Long.MIN_VALUE+17 : 0));
                require(value.zeroFlag==changed && value.zeroArray==(changed ? array : null));
            }
            require(Trace.seenInput==(last==8 ? input : null));
            System.out.println("route="+route+",variant="+variant+",mode="+mode+",attempt="+attempt+",events="+expected);
        }
    }
}
