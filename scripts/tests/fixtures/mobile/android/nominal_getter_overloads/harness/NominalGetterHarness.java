import fixture.*;
import java.util.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class NominalGetterHarness {
    static void require(boolean value) { if(!value)throw new AssertionError("owned nominal getter overload changed"); }
    static Object call(Core receiver,Object key,int slot,int route,String alias) throws Throwable {
        if(route==0) {
            if(slot==0)return NominalGetterCaller.generic(receiver,(Key)key);
            if(slot==3)return NominalGetterCaller.mapped(receiver,key);
            return NominalGetterCaller.exact(receiver,key,slot,alias);
        }
        if(route==1) {
            if(slot==0)return ((Reader)receiver).get((Key)key);
            if(slot==3)return ((Map)receiver).get(key);
            return NominalGetterCaller.exact(receiver,key,slot,alias);
        }
        Class<?> parameter=slot<2?Key.class:Object.class;
        Class<?> result=slot==1||slot==2?Box.class:Object.class;
        String name=slot==1?alias:"get";
        if(route==2) {
            Method selected=null;
            for(Method method:Core.class.getDeclaredMethods())
                if(method.getName().equals(name)&&method.getReturnType()==result&&Arrays.equals(method.getParameterTypes(),new Class<?>[]{parameter}))selected=method;
            require(selected!=null);
            try{return selected.invoke(receiver,key);}catch(InvocationTargetException error){throw error.getCause();}
        }
        return MethodHandles.publicLookup().findVirtual(Core.class,name,MethodType.methodType(result,parameter)).invoke(receiver,key);
    }
    @SuppressWarnings({"rawtypes","unchecked"})
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);String alias=args[2];
        Core receiver=mode==12?null:new Core();
        Key typed=mode==3?null:new Key(new Object());Object dynamic=mode==4?new Object():typed;
        Object sentinel=new Object();RuntimeException exact=new RuntimeException("owned getter failure");
        for(int attempt=0;attempt<2;++attempt) {
            Box box=new Box(new Object());Object stored=mode==5?new Object():mode==6?null:box;
            Object read=mode==7?null:mode==8?stored:mode==9?typed:mode==10?receiver:new Object();
            if(receiver!=null) {receiver.clear();if(mode!=2)((Map)receiver).put(typed,stored);if(mode==4)((Map)receiver).put(dynamic,new Object());}
            for(int slot=0;slot<4;++slot) {
                Trace.hits=0;Trace.seen=sentinel;Trace.result=read;Trace.failure=mode==1?exact:null;
                Object key=slot<2?typed:dynamic;Object result=null;Throwable failure=null;
                try{result=call(receiver,key,slot,route,alias);}catch(Throwable error){failure=error;}
                boolean generic=slot==0,body=generic&&receiver!=null&&typed!=null;
                require(Trace.hits==(body?1:0)&&Trace.seen==(body?typed:sentinel));
                if(receiver==null||(generic&&typed==null))require(failure instanceof NullPointerException);
                else if(body&&mode==1)require(failure==exact);
                else {
                    boolean guarded=slot>=2 && !(dynamic instanceof Key);
                    Object expected=generic?read:guarded||mode==2?null:stored;
                    if(!generic&&!guarded&&mode!=2&&stored!=null&&!(stored instanceof Box))require(failure instanceof ClassCastException);
                    else require(failure==null&&result==expected);
                }
                if(receiver!=null)require(receiver.size()==(mode==2?0:mode==4?2:1));
            }
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",operations=4");
        }
    }
}
