import fixture.*;
import java.util.*;
import java.lang.reflect.*;
import java.lang.invoke.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class ScopedScalarPairFactoryHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned scoped scalar pair factory"); }
    static final class OpaqueMap extends AbstractMap {
        Object key,value;int reads;
        public Object get(Object key) { require(key==this.key);reads++;return value; }
        public Set keySet() { reads++;return Collections.singleton(key); }
        public Set entrySet() { throw new AssertionError("owned scoped scalar pair factory traversed map"); }
    }
    static final class Action implements Runnable {
        int calls;Object replacement;
        public void run() { Trace.hit(4);calls++;if(replacement!=null)Trace.result=replacement; }
    }
    static Object call(int route,int operation,boolean flag) throws Throwable {
        if(route==0)return operation==0?Client.call(flag):operation==1?Client.value(flag):Client.key(flag);
        if(route==1)return operation==0?ScopedScalarPairFactoryCaller.call(flag):operation==1?ScopedScalarPairFactoryCaller.value(flag):ScopedScalarPairFactoryCaller.key(flag);
        String name=operation==0?"call":operation==1?"value":"key";
        if(route==2) {
            try { return Client.class.getMethod(name,boolean.class).invoke(null,flag); }
            catch(InvocationTargetException error) { throw error.getCause(); }
        }
        if(operation==0)return (Object)MethodHandles.publicLookup().findStatic(Client.class,name,MethodType.methodType(Object.class,boolean.class)).invokeExact(flag);
        return (String)MethodHandles.publicLookup().findStatic(Client.class,name,MethodType.methodType(String.class,boolean.class)).invokeExact(flag);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);boolean flag=(mode&1)!=0;
        TypeVariable<?>[] own=Factory.class.getMethod("create",boolean.class).getTypeParameters(),owner=Factory.class.getTypeParameters();
        require(own.length==2&&owner.length==2&&!own[0].equals(owner[0])&&!own[1].equals(owner[1]));
        require(Arrays.equals(((ParameterizedType)Factory.class.getMethod("create",boolean.class).getGenericReturnType()).getActualTypeArguments(),own));
        Trace.failAt=mode>=1&&mode<=4?mode:0;Trace.failure=new RuntimeException("owned scalar failure");
        OpaqueMap map=new OpaqueMap();map.key=mode==8?new Object():new String("owned key");map.value=mode==9?new Object():new String("owned value");Trace.key=map.key;
        Factory box=new Factory(mode==10?null:map),replacement=new Factory(map);Action action=new Action();
        action.replacement=mode==11?replacement:null;Trace.action=mode==5?null:action;
        Object initial=mode==6?null:mode==7?new Object():box;Object sentinel=new Object();
        for(int attempt=0;attempt<2;attempt++) {
            Trace.events=0;Trace.result=initial;Trace.after=sentinel;Trace.capacitySeen=-7;Trace.flagSeen=!flag;
            int calls=action.calls,reads=map.reads;Object actual=null;Throwable failure=null;
            try { actual=call(route,operation,flag); } catch(Throwable error) { failure=error; }
            long expected=mode==1?1:mode==4?14:mode==5?1:mode==2||mode==7?142:1423;
            require(Trace.events==expected&&Trace.capacitySeen==(mode==1?-7:operation==2?59:37));
            require(Trace.flagSeen==(mode==1?!flag:flag));
            boolean ran=mode!=1&&mode!=4&&mode!=5;require(action.calls==calls+(ran?1:0));
            Object result=mode==11?replacement:initial;require(Trace.result==result);
            boolean returned=mode!=1&&mode!=2&&mode!=4&&mode!=5&&mode!=7;require(Trace.after==(returned?result:sentinel));
            boolean read=returned&&mode!=3&&mode!=6&&mode!=10&&operation!=0;
            require(map.reads==reads+(read?1:0));
            if(mode>=1&&mode<=4)require(failure==Trace.failure);
            else if(mode==5||operation!=0&&(mode==6||mode==10))require(failure instanceof NullPointerException);
            else if(mode==7||operation==1&&mode==9||operation==2&&mode==8)require(failure instanceof ClassCastException);
            else require(failure==null&&actual==(operation==0?result:operation==1?map.value:map.key));
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
