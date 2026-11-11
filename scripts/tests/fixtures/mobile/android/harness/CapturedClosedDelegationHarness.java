import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class CapturedClosedDelegationHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned captured closed delegation"); }
    static Class<?> worker;
    static boolean access;
    static Object field(Object value,String name) throws Throwable {
        Field field=worker.getDeclaredField(name);field.setAccessible(true);return field.get(value);
    }
    static void metadata(boolean projected,boolean publicMember,boolean synthetic) throws Throwable {
        worker=Class.forName("fixture.Owner$Worker",false,Owner.class.getClassLoader());
        require(worker.getDeclaringClass()==Owner.class && worker.getEnclosingClass()==Owner.class);
        require(worker.getTypeParameters().length==0 && Owner.class.getTypeParameters().length==0);
        require(Modifier.isStatic(worker.getModifiers())==projected && Modifier.isFinal(worker.getModifiers()));
        require(Modifier.isPublic(worker.getModifiers())==publicMember && Modifier.isPrivate(worker.getModifiers())!=publicMember);
        Field capture=worker.getDeclaredField("this$0");
        require(capture.getType()==Owner.class && capture.isSynthetic()!=projected && Modifier.isFinal(capture.getModifiers()));
        for(boolean wide:new boolean[]{false,true}) {
            Constructor<?> ctor=wide?worker.getDeclaredConstructor(Owner.class,int.class,long.class,Callback.class)
                :worker.getDeclaredConstructor(Owner.class,int.class,Callback.class);
            require(ctor.getTypeParameters().length==0 && Modifier.isPrivate(ctor.getModifiers())==(access||!wide));
            Type[] types=ctor.getGenericParameterTypes();int offset=projected?1:0;
            require(types.length==(wide?3:2)+offset && (!projected || types[0]==Owner.class));
            require(types[offset]==int.class && (!wide || types[offset+1]==long.class));
            ParameterizedType callback=(ParameterizedType)types[types.length-1];
            require(callback.getRawType()==Callback.class);
            WildcardType input=(WildcardType)callback.getActualTypeArguments()[0],output=(WildcardType)callback.getActualTypeArguments()[1];
            require(input.getLowerBounds().length==1 && input.getLowerBounds()[0]==Value.class && input.getUpperBounds()[0]==Object.class);
            require(output.getLowerBounds().length==0 && output.getUpperBounds()[0]==Value.class);
        }
        if(access) {
            Constructor<?> raw=worker.getDeclaredConstructor(Owner.class,int.class,long.class,Callback.class,Object.class);
            require(Modifier.isPublic(raw.getModifiers()) && raw.isSynthetic()==synthetic && raw.getTypeParameters().length==0);
            require(java.util.Arrays.equals(raw.getGenericParameterTypes(),raw.getParameterTypes()));
        }
    }
    static Object call(int route,Owner owner,int index,long bits,Callback callback) throws Throwable {
        if(route==0)return owner.create(index,bits,callback);
        if(route==1)return CapturedClosedDelegationCaller.call(owner,index,bits,callback);
        Constructor<?> ctor=access?worker.getDeclaredConstructor(Owner.class,int.class,long.class,Callback.class,Object.class)
            :worker.getDeclaredConstructor(Owner.class,int.class,long.class,Callback.class);ctor.setAccessible(true);
        if(route==2)try{return access?ctor.newInstance(owner,index,bits,callback,null):ctor.newInstance(owner,index,bits,callback);}catch(InvocationTargetException error){throw error.getCause();}
        if(access)return (Object)MethodHandles.lookup().unreflectConstructor(ctor).asType(MethodType.methodType(Object.class,Owner.class,int.class,long.class,Callback.class,Object.class)).invokeExact(owner,index,bits,callback,(Object)null);
        return (Object)MethodHandles.lookup().unreflectConstructor(ctor).asType(MethodType.methodType(Object.class,Owner.class,int.class,long.class,Callback.class)).invokeExact(owner,index,bits,callback);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        access=args.length>4 && Boolean.parseBoolean(args[4]);
        metadata(args[2].equals("rebuilt"),Boolean.parseBoolean(args[3]),access&&args[2].equals("encoded"));
        Owner owner=mode==16?null:new Owner();
        RuntimeException failure=new IllegalArgumentException("owned captured constructor failure");Trace.failure=failure;
        Trace.value=mode==3?null:new Value();
        int stop=mode>=7&&mode<=15?mode-6:mode==1?4:mode==2?5:0;
        int[] calls={0};
        for(int attempt=0;attempt<2;attempt++) {
            Trace.events=0;Trace.partial=null;Trace.failAt=mode>=7&&mode<=15?stop:0;
            Value expected=mode==4?null:new Value();
            Callback<Value,Value> hook=value->{require(value==Trace.value);calls[0]++;Trace.hit(5);if(mode==2)throw failure;return expected;};
            Callback callback=mode==1?null:hook;
            int index=mode==5?Integer.MIN_VALUE:mode==6?Integer.MAX_VALUE:17+attempt;
            long bits=mode==5?Long.MIN_VALUE:mode==6?Long.MAX_VALUE:0x123456789abcdefL+attempt;
            Object result=null;Throwable caught=null;int before=calls[0];
            try{result=call(route,owner,index,bits,callback);}catch(Throwable error){caught=error;}
            boolean nullFactory=mode==16&&route<2,failedInit=mode==7;
            long events=0;
            if(!nullFactory && !(failedInit&&attempt==1))
                for(int event=attempt==0?1:2;event<=9;event++){events=events*10+event;if(event==stop)break;}
            require(Trace.events==events);
            if(nullFactory || mode==1)require(caught instanceof NullPointerException);
            else if(failedInit)require(attempt==0?caught instanceof ExceptionInInitializerError&&caught.getCause()==failure:caught instanceof NoClassDefFoundError);
            else if(stop!=0)require(caught==failure);
            else require(caught==null && result==Trace.partial && result!=null);
            boolean entered=!nullFactory && !failedInit;
            require((Trace.partial!=null)==entered);
            if(entered) {
                Method outer=worker.getDeclaredMethod("owner");outer.setAccessible(true);
                require(outer.invoke(Trace.partial)==owner && field(Trace.partial,"this$0")==owner);
                require(((Integer)field(Trace.partial,"index")).intValue()==(stop==0||stop>=3?index:0));
                require(field(Trace.partial,"callback")==((stop==0||stop>=4)?callback:null));
                require(field(Trace.partial,"value")==((stop==0||stop>=7)?expected:null));
                require(((Long)field(Trace.partial,"bits")).longValue()==((stop==0||stop>=9)?bits:0));
            }
            require(calls[0]==before+(entered&&(stop==0||stop>=5)?1:0));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
