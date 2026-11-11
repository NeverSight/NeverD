import fixture.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
public final class AncestorSuperCallHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned ancestor super call"); }
    public static final class Future extends Core<String,String> implements Cloneable {
        final Object tag=new Object();
        public Future(Object token,Object value) { super(token,value); }
        protected Object clone() { Trace.hit(9);return this; }
    }
    static Core<?,?> call(int route,Core<String,String> value) throws Throwable {
        if(route==0)return value.copy();
        if(route==1)return AncestorSuperCallCaller.copy(value);
        if(route==2) {
            try { return (Core<?,?>)Core.class.getMethod("copy").invoke(value); }
            catch(InvocationTargetException error) { throw error.getCause(); }
        }
        return (Core<?,?>)MethodHandles.publicLookup().findVirtual(Core.class,"copy",MethodType.methodType(Core.class)).invokeExact((Core)value);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),kind=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        Object token=new Object(),initial=new String("owned initial"),sentinel=new Object();
        Core<String,String> value=kind==0?new Core<String,String>(token,initial):new Future(token,initial);
        Trace.failure=new RuntimeException("owned clone failure");Trace.failAt=mode==1||mode==2?mode:0;
        Trace.result=mode==5?null:mode==6?new Object():mode==4?initial:new String("owned copy");
        Object previous=null;Throwable previousFailure=null;
        for(int attempt=0;attempt<2;attempt++) {
            Trace.events=0;Trace.partial=sentinel;int created=Trace.created;Throwable failure=null;Core<?,?> copy=null;Object actual=null;
            try { copy=call(route,value);actual=(String)copy.value; } catch(Throwable error) { failure=error; }
            require(Trace.created==created&&value.token==token&&value.value==initial&&value.back==value);
            if(kind==0) {
                require(failure instanceof InternalError&&failure.getCause() instanceof CloneNotSupportedException&&failure!=previousFailure);
                require(Trace.events==0&&Trace.partial==sentinel&&copy==null);previousFailure=failure;
            } else {
                require(Trace.events==(mode==1?1:12));
                if(mode==1)require(Trace.partial==sentinel);
                else {
                    require(Trace.partial instanceof Future&&Trace.partial!=value&&Trace.partial!=previous);
                    Core<?,?> partial=(Core<?,?>)Trace.partial;
                    require(partial.getClass()==value.getClass()&&partial.token==token&&partial.back==value&&partial.value==Trace.result);
                    require(((Future)partial).tag==((Future)value).tag);previous=partial;
                }
                if(mode==1||mode==2)require(failure==Trace.failure&&copy==null);
                else if(mode==6)require(failure instanceof ClassCastException&&copy==Trace.partial);
                else require(failure==null&&copy==Trace.partial&&actual==Trace.result);
            }
            System.out.println("route="+route+",operation="+kind+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
