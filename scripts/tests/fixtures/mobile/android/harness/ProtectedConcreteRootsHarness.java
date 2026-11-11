import fixture.*;
import java.lang.reflect.*;
public final class ProtectedConcreteRootsHarness {
    static void require(boolean condition){if(!condition)throw new AssertionError("owned protected concrete roots");}
    static Object create(int kind){switch(kind){case 0:return new PlainChild();case 1:return new OverrideChild();case 2:return new PlainPublic();default:return new PublicChild();}}
    static long stored(Object value){return value instanceof Parent?((Parent)value).getStored():((PublicPeer)value).getStored();}
    static long chosen(Object value){return value instanceof OverrideChild?((OverrideChild)value).getChosen():value instanceof PublicChild?((PublicChild)value).getChosen():0;}
    static Method metadata(Class<?> owner,String name,boolean visible)throws Exception{
        Method method=owner.getDeclaredMethod(name,long.class);int access=method.getModifiers();require((visible?Modifier.isPublic(access):Modifier.isProtected(access)) && !Modifier.isStatic(access) && !Modifier.isAbstract(access));
        require(method.getTypeParameters().length==0 && method.getReturnType()==void.class && method.getGenericParameterTypes()[0]==long.class);method.setAccessible(true);return method;
    }
    public static void main(String[] args)throws Throwable{
        int kind=Integer.parseInt(args[0]),route=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        Method parent=metadata(Parent.class,args[3],false),peer=metadata(PublicPeer.class,args[5],true);metadata(OverrideChild.class,args[4],false);metadata(PublicChild.class,args[6],true);
        require(!Modifier.isFinal(parent.getModifiers()) && !Modifier.isFinal(peer.getModifiers()));require(Modifier.isAbstract(Parent.class.getModifiers()) && Modifier.isAbstract(PublicPeer.class.getModifiers()));
        long[] values={0,1,-1,Long.MIN_VALUE,Long.MAX_VALUE,0x0123456789ABCDEFL,0x7FFF000080000001L,0x800000007FFFFFFFL};Throwable previous=null;
        for(int attempt=0;attempt<2;attempt++){
            Trace.calls=Trace.phase=Trace.tag=Trace.fail=0;Trace.receiver=null;Trace.bits=0;
            Throwable failure=mode==9?new AssertionError("owned exact error"):mode==10?new Exception("owned exact checked failure"):new IllegalArgumentException("owned exact failure");if(mode==12)failure=null;Trace.failure=failure;Trace.fail=mode==8 || mode==10?1:mode==9 || mode==12?2:0;
            long bits=values[mode<8?mode:mode==8 || mode==10?3:4];Object first=create(kind),second=create(kind),receiver=attempt==0?first:second;if(mode==11)receiver=null;
            Throwable seen=null;try{if(route==0){if(kind<2)((Parent)receiver).send(bits);else((PublicPeer)receiver).send(bits);}else{try{(kind<2?parent:peer).invoke(receiver,bits);}catch(InvocationTargetException error){throw error.getCause();}}}catch(Throwable error){seen=error;}
            if(mode==11){require(seen!=null && seen.getClass()==NullPointerException.class && seen!=previous);require(Trace.calls==0 && Trace.phase==0 && Trace.tag==0 && Trace.receiver==null && Trace.bits==0);}
            else {
                boolean early=mode==8 || mode==10;require(Trace.calls==1 && Trace.phase==(early?1:2) && Trace.tag==kind+1 && Trace.receiver==receiver && Trace.bits==bits);
                if(mode==8 || mode==9 || mode==10)require(seen==failure);else if(mode==12)require(seen!=null && seen.getClass()==NullPointerException.class && seen!=previous);else require(seen==null);
                require(stored(receiver)==(kind%2==0 && !early?bits:0));require(chosen(receiver)==(kind%2==1 && !early?bits:0));
            }
            Object other=attempt==0?second:first;require(stored(other)==0 && chosen(other)==0);previous=seen;
        }
        System.out.println("attempts=2,protected-concrete-roots=matched");
    }
}
