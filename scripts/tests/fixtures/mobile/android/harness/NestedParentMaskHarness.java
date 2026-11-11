package fixture;
import java.lang.invoke.*;
import java.lang.reflect.*;
import java.util.*;
import kotlin.jvm.internal.DefaultConstructorMarker;

public final class NestedParentMaskHarness {
    static void check(boolean ok) { if(!ok)throw new AssertionError("owned nested parent mask"); }
    static Core<?> make(int route,int count,long bits,Sink<?> callback,int mask) throws Throwable {
        try {
            if(route==0)return Client.make(count,bits,callback,mask);
            if(route==1)return NestedParentMaskCaller.make(count,bits,callback,mask);
            if(route==2)return (Core<?>)Core.class.getDeclaredConstructor(int.class,long.class,Sink.class,int.class,
                    DefaultConstructorMarker.class).newInstance(count,bits,callback,mask,null);
            return (Core<?>)MethodHandles.lookup().findConstructor(Core.class,MethodType.methodType(void.class,
                    int.class,long.class,Sink.class,int.class,DefaultConstructorMarker.class))
                    .invoke(count,bits,callback,mask,(DefaultConstructorMarker)null);
        } catch(InvocationTargetException error) { throw error.getCause(); }
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);boolean synthetic=Boolean.parseBoolean(args[2]);
        Constructor<?> ctor=Core.class.getDeclaredConstructor(int.class,long.class,Sink.class,int.class,DefaultConstructorMarker.class);
        check(ctor.isSynthetic()==synthetic&&ctor.getGenericParameterTypes()[2]==Sink.class);
        check(!Modifier.isPublic(Core.class.getModifiers())&&!Modifier.isPublic(Local.class.getModifiers()));
        ParameterizedType parent=(ParameterizedType)Core.class.getGenericSuperclass();
        ParameterizedType argument=(ParameterizedType)parent.getActualTypeArguments()[0];
        check(parent.getRawType()==Parent.class&&argument.getRawType()==Local.class);
        check(argument.getActualTypeArguments()[0].equals(Core.class.getTypeParameters()[0]));
        check(Parent.class.getTypeParameters()[0].getBounds()[0]==Bound.class&&View.class.isAssignableFrom(Core.class));
        int scenario=mode/4,mask=(mode%4)*2;
        if(scenario==15)mask|=0xfffffff8;
        int count=scenario==14?Integer.MIN_VALUE:scenario==15?Integer.MAX_VALUE:29;
        long bits=scenario==12?Long.MIN_VALUE:scenario==13?Long.MAX_VALUE:0x1234567812345678L;
        long defaults=bits^0x1234567800000011L;
        Sink<Object> incoming=scenario==10?null:new Consumer();Object token=scenario==11?null:new Object();
        boolean hasBits=(mask&2)!=0,hasCallback=(mask&4)!=0,usesDefaults=hasBits||hasCallback;
        boolean ready=false,broken=false;
        for(int attempt=0;attempt<2;attempt++) {
            Object sentinel=new Object();Trace.events=0;Trace.parent=null;Trace.partial=null;
            Trace.seen=sentinel;Trace.callback=sentinel;Trace.token=token;Trace.defaultBits=defaults;
            Trace.failAt=scenario>=1&&scenario<=9?scenario:0;Trace.failure=new RuntimeException("owned nested mask failure");
            boolean sticky=usesDefaults&&broken;
            ArrayList<Integer> steps=new ArrayList<Integer>();
            if(!sticky) {
                if(usesDefaults&&!ready) { steps.add(1);steps.add(2); }
                if(hasBits)steps.add(3);
                for(int step=4;step<=7;step++)steps.add(step);
                if(hasCallback||incoming!=null) { steps.add(8);steps.add(9); }
            }
            boolean[] seen=new boolean[10];long expected=0;int last=0;
            for(int step:steps) { seen[step]=true;last=step;expected=expected*10+step;if(step==Trace.failAt)break; }
            if(seen[2]&&Trace.failAt!=2)ready=true;
            boolean initFailure=(last==1||last==2)&&last==Trace.failAt;
            if(initFailure)broken=true;
            boolean injected=Trace.failAt>0&&last==Trace.failAt;
            boolean nullCallback=!sticky&&!injected&&!hasCallback&&incoming==null;
            Throwable caught=null;Core<?> result=null;
            try { result=make(route,count,bits,incoming,mask); }catch(Throwable error) { caught=error; }
            check(Trace.events==expected&&(Trace.parent!=null)==seen[4]&&(Trace.partial!=null)==seen[6]);
            if(seen[4])check(((Parent<?>)Trace.parent).boot==(seen[5]?17:0));
            if(seen[6]) {
                Core<?> partial=(Core<?>)Trace.partial;
                check(partial==Trace.parent&&partial.count==(seen[7]?count:0));
                check(partial.bits==(seen[7]?(hasBits?defaults:bits):0));
                check(partial.callback==(seen[7]?(hasCallback?Defaults.CB:incoming):null));
            }
            check(Trace.seen==(seen[8]?token:sentinel));
            check(Trace.callback==(seen[8]?(hasCallback?Defaults.CB:incoming):sentinel));
            if(sticky)check(caught instanceof NoClassDefFoundError);
            else if(initFailure)check(caught instanceof ExceptionInInitializerError&&caught.getCause()==Trace.failure);
            else if(injected)check(caught==Trace.failure);
            else if(nullCallback)check(caught instanceof NullPointerException);
            else check(caught==null&&result==Trace.partial);
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
