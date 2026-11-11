import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class TypedCallbackConstructorHarness {
    static Object input;
    static Bound velocity;
    static int callbacks;
    static void check(boolean ok) { if(!ok)throw new AssertionError("owned typed callback constructor behavior"); }
    static final class Factory implements Spec<Object> {
        final Vector vector;final Link expected;
        Factory(Vector vector,Link expected) { this.vector=vector;this.expected=expected; }
        @SuppressWarnings("unchecked")
        public <R extends Bound> Vector<R> vectorize(Link<Object,R> converter) {
            check(converter==expected);Trace.converter=converter;Trace.conversions++;Trace.hit(1);return vector;
        }
    }
    static final class Invoke implements Callback {
        final Object result;
        Invoke(Object result) { this.result=result; }
        public Object apply(Object value) { check(value==input);callbacks++;Trace.hit(7);return result; }
    }
    static Core create(int route,Spec spec,Link converter,Object first,Object value) throws Throwable {
        if(route==0)return Client.create(spec,converter,first,value);
        if(route==1)return TypedCallbackConstructorCaller.create(spec,converter,first,value);
        if(route==2) {
            try { return Core.class.getConstructor(Spec.class,Link.class,Object.class,Object.class).newInstance(spec,converter,first,value); }
            catch(InvocationTargetException e) { throw e.getCause(); }
        }
        MethodHandle h=MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Spec.class,Link.class,Object.class,Object.class));
        return (Core)h.invokeExact(spec,converter,first,value);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        for(int attempt=0;attempt<2;attempt++) {
            Object first=mode==2?null:new Object();input=mode==16?null:new Object();
            velocity=mode==3||mode==15?null:new Bound();callbacks=0;
            Link converter=mode==4?null:new Link<Object,Bound>();Vector vector=mode==5?null:new Vector<Bound>(new Bound());
            Spec spec=mode==1?null:new Factory(vector,converter);
            Trace.events=0;Trace.conversions=0;Trace.partial=null;Trace.converter=null;
            Trace.failAt=mode>=6&&mode<=10?mode-5:mode==11?6:mode==12?7:0;
            Trace.failure=new RuntimeException("owned failure");
            Trace.callback=mode==13?null:new Invoke(mode==14?new Object():velocity);
            Core result=null;Throwable caught=null;
            try { result=create(route,spec,converter,first,input); }catch(Throwable e) { caught=e; }
            if(mode==1||mode==4||mode==13)check(caught instanceof NullPointerException);
            else if(mode==14)check(caught instanceof ClassCastException);
            else if(Trace.failAt!=0)check(caught==Trace.failure);
            else check(caught==null&&result==Trace.partial);
            check(Trace.conversions==(mode==1?0:1));
            if(mode!=1)check(Trace.converter==converter);
            boolean started=mode!=1&&mode!=4&&mode!=6&&mode!=11&&mode!=12&&mode!=13&&mode!=14;
            check((Trace.partial!=null)==started);
            int expected=mode==1?0:1;
            if(mode!=1&&mode!=4&&mode!=6)expected=16;
            if(mode!=1&&mode!=4&&mode!=6&&mode!=11&&mode!=13)expected=167;
            if(started) {
                for(int stage=2;stage<=5;stage++) { expected=expected*10+stage;if(Trace.failAt==stage)break; }
                check(Trace.partial.vector==(mode==7?null:vector));
                check(Trace.partial.converter==(mode==7||mode==8?null:converter));
                check(Trace.partial.first()==(mode==7||mode==8?null:first));
                check(((View)Trace.partial).velocity()==(mode>=7&&mode<=9?null:velocity));
            }
            check(Trace.events==expected);
            check(callbacks==(mode==1||mode==4||mode==6||mode==11||mode==13?0:1));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
