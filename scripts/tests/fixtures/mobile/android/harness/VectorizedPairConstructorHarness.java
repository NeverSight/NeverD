import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class VectorizedPairConstructorHarness {
    static void check(boolean ok) { if(!ok)throw new AssertionError("owned vectorized pair constructor behavior"); }
    static final class Factory implements Spec<Object> {
        final Vector vector;final Link expected;
        Factory(Vector vector,Link expected) { this.vector=vector;this.expected=expected; }
        @SuppressWarnings("unchecked")
        public <R extends Bound> Vector<R> vectorize(Link<Object,R> converter) {
            check(converter==expected);Trace.converter=converter;Trace.conversions++;Trace.hit(1);return vector;
        }
    }
    static Core create(int route,Spec spec,Link converter,Object first,Object second,Bound velocity) throws Throwable {
        if(route==0)return Client.create(spec,converter,first,second,velocity);
        if(route==1)return VectorizedPairConstructorCaller.create(spec,converter,first,second,velocity);
        if(route==2) {
            try { return Core.class.getConstructor(Spec.class,Link.class,Object.class,Object.class,Bound.class).newInstance(spec,converter,first,second,velocity); }
            catch(InvocationTargetException e) { throw e.getCause(); }
        }
        MethodHandle h=MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Spec.class,Link.class,Object.class,Object.class,Bound.class));
        return (Core)h.invokeExact(spec,converter,first,second,velocity);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        TypeVariable<?>[] own=Core.class.getTypeParameters();check(own.length==2&&own[0].getBounds()[0]==Object.class&&own[1].getBounds()[0]==Bound.class);
        for(int attempt=0;attempt<2;attempt++) {
            Object first=mode==2?null:new Object(),second=mode==3?null:new String("owned distinct second "+attempt);Bound velocity=mode==3?null:new Bound();
            Link converter=mode==4?null:new Link<Object,Bound>();Vector vector=mode==5?null:new Vector<Bound>(new Bound());
            Spec spec=mode==1?null:new Factory(vector,converter);
            Trace.events=0;Trace.conversions=0;Trace.partial=null;Trace.converter=null;
            Trace.failAt=mode>=6&&mode<=10?mode-5:0;Trace.failure=new RuntimeException("owned failure");
            Core result=null;Throwable caught=null;
            try { result=create(route,spec,converter,first,second,velocity); }catch(Throwable e) { caught=e; }
            if(mode==1)check(caught instanceof NullPointerException);
            else if(Trace.failAt!=0)check(caught==Trace.failure);
            else check(caught==null&&result==Trace.partial);
            check(Trace.conversions==(mode==1?0:1));
            if(mode!=1)check(Trace.converter==converter);
            long expected=mode==1?0:mode==6?1:mode==7?12:mode==8?123:mode==9?1234:12345;
            check(Trace.events==expected);
            boolean partial=mode!=1&&mode!=6;check((Trace.partial!=null)==partial);
            if(partial) {
                check(Trace.partial.vector==(mode==7?null:vector));
                check(Trace.partial.converter==(mode==7||mode==8?null:converter));
                check(Trace.partial.first()==(mode==7||mode==8?null:first));
                check(Trace.partial.second==(mode==7||mode==8?null:second));
                check(((View)Trace.partial).velocity()==(mode>=7&&mode<=9?null:velocity));
            }
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
