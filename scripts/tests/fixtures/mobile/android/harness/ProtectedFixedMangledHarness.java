import api.*;
import fixture.*;
import other.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ProtectedFixedMangledHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned protected fixed mangled"); }
    static Object call(int route,Child receiver,Press scope,long bits,Sink<? super Value> sink,String name) throws Throwable {
        if(route==0)return receiver.run(scope,bits,sink);
        if(route==1)return ProtectedFixedMangledCaller.call(receiver,scope,bits,sink);
        if(route==2) {
            Method m=Owner.class.getDeclaredMethod(name,Press.class,long.class,Sink.class);m.setAccessible(true);
            try { return m.invoke(receiver,scope,bits,sink); }catch(InvocationTargetException e) { throw e.getCause(); }
        }
        if(route==3)return (Object)MethodHandles.publicLookup().findVirtual(Child.class,"run",MethodType.methodType(Object.class,Press.class,long.class,Sink.class)).invokeExact(receiver,scope,bits,(Sink)sink);
        if(route==4)return Peer.call(receiver,scope,bits,sink);
        return ProtectedFixedMangledCaller.peer(receiver,scope,bits,sink);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);String name=args[2];
        Method m=Owner.class.getDeclaredMethod(name,Press.class,long.class,Sink.class);
        require(Modifier.isAbstract(Owner.class.getModifiers())&&Owner.class.getTypeParameters().length==0&&Owner.class.getSuperclass()==Base.class);
        require(Modifier.isProtected(m.getModifiers())&&Modifier.isFinal(m.getModifiers())&&!Modifier.isStatic(m.getModifiers())&&m.getTypeParameters().length==0&&m.getReturnType()==Object.class&&m.getExceptionTypes().length==0);
        Type arg=((ParameterizedType)m.getGenericParameterTypes()[2]).getActualTypeArguments()[0];
        require(arg instanceof WildcardType&&((WildcardType)arg).getLowerBounds().length==1&&((WildcardType)arg).getLowerBounds()[0]==Value.class);
        int[] failures={0,0,0,0,0,1,4,2,5,3,6,0,0};
        for(int attempt=0;attempt<2;attempt++) {
            Trace.constructed=0;Child receiver=new Child();require(Trace.constructed==1);
            long bits=mode==3?Long.MIN_VALUE:mode==4?Long.MAX_VALUE:0x8123456789abcdefL+attempt;
            Object initial=mode==4?null:new Object(),changed=new Object();Value value=mode==3?null:new Value();
            Press scope=mode==1?null:new Press() { public void touch() { require(Trace.receiver==receiver&&Trace.scope==this&&Trace.bits==bits);Trace.touched=this;if(mode==11)Trace.output=changed;Trace.hit(4); } };
            Sink<Object> sink=mode==2?null:new Sink<Object>() { public void accept(Object received) { require(received==value&&Trace.sink==this&&Trace.receiver==receiver);Trace.accepted=received;if(mode==12)Trace.output=null;Trace.hit(5); } };
            Object expected=mode==11?changed:mode==12?null:initial;
            Trace.events=0;Trace.bits=0;Trace.receiver=Trace.scope=Trace.sink=Trace.touched=Trace.accepted=Trace.result=null;
            Trace.output=initial;Trace.value=value;Trace.failAt=failures[mode];Trace.failure=new RuntimeException("owned failure");
            Object actual=null;Throwable caught=null;
            try { actual=call(route,receiver,scope,bits,sink,name); }catch(Throwable e) { caught=e; }
            if(mode==1||mode==2)require(caught instanceof NullPointerException);else if(Trace.failAt!=0)require(caught==Trace.failure);else require(caught==null&&actual==expected);
            long events=mode==1||mode==5?1:mode==6?14:mode==2||mode==7?142:mode==8?1425:mode==9?14253:142536;
            require(Trace.events==events&&Trace.receiver==receiver&&Trace.scope==scope&&Trace.sink==sink&&Trace.bits==bits);
            boolean touched=mode!=1&&mode!=5,accepted=touched&&mode!=2&&mode!=6&&mode!=7,result=accepted&&mode!=8&&mode!=9;
            require(Trace.touched==(touched?scope:null)&&Trace.accepted==(accepted?value:null)&&Trace.result==(result?expected:null));
            require(Trace.output==(mode==11?changed:mode==12?null:initial));
            require(Trace.constructed==1);
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+events);
        }
    }
}
