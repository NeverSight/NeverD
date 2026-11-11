import fixture.*;
import java.io.IOException;
import java.util.Map;
import java.util.Collection;
import java.util.HashMap;
import java.util.ArrayList;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ScopedIntegerGetterDelegationHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned scoped integer getter delegation");}
    @SuppressWarnings("unchecked") private static <X extends Throwable> void fail(Throwable failure)throws X{throw (X)failure;}
    private static final class Input extends HashMap<String,Collection<Object>> {
        public int size(){Trace.reads++;Trace.readSource=this;Trace.hit(1);int size=Trace.nextSize;Trace.readAction.run();return size;}
    }
    @SuppressWarnings("unchecked") private static Core<String,Object> call(int route,int operation,float factor,Map<String,Collection<Object>> value,Maker<Collection<Object>> maker)throws Throwable {
        if(route==0)return operation==0?new Core<String,Object>(factor,value,maker):new Core<String,Object>(value,factor,maker);
        if(route==1)return operation==0?ScopedIntegerGetterDelegationCaller.first(factor,value,maker):ScopedIntegerGetterDelegationCaller.second(factor,value,maker);
        if(route==2){try{return (Core)(operation==0?Core.class.getConstructor(float.class,Map.class,Maker.class).newInstance(factor,value,maker):Core.class.getConstructor(Map.class,float.class,Maker.class).newInstance(value,factor,maker));}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,float.class,Map.class,Maker.class)).invokeExact(factor,value,maker);
        return (Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Map.class,float.class,Maker.class)).invokeExact(value,factor,maker);
    }
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final RuntimeException runtime=new IllegalArgumentException("owned failure");final Error error=new Error("owned error");final IOException checked=new IOException("owned checked");
        Trace.failure=new IllegalArgumentException("owned stage");Trace.failAt=mode==6?1:mode==7?2:mode==9?4:0;
        final float factor=mode==2?Float.intBitsToFloat(0x7fc01234):0.625f;final int size=mode==3?-17:13;
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object(),payload=new Object();final Collection<Object> selected=new ArrayList<Object>();final Object expected=mode==11?Integer.valueOf(37):selected;
            final Map<String,Collection<Object>> value=mode==1?null:new Input(),other=new HashMap<String,Collection<Object>>();
            Trace.events=0;Trace.nextSize=size;Trace.snapshot=0;Trace.readSource=null;Trace.afterSource=other;Trace.factory=null;Trace.created=sentinel;Trace.instance=null;Trace.payload=sentinel;Trace.factor=0;
            final int reads=Trace.reads,targets=Trace.targets,calls=Trace.calls,wrong=Trace.wrong;
            final Maker<Collection<Object>> maker=new Maker<Collection<Object>>(){public Object make(){Trace.calls++;require(Trace.events==182&&Trace.snapshot==size);Trace.hit(6);if(mode==8)throw runtime;return expected;}};
            Trace.readAction=new Runnable(){public void run(){require(Trace.events==1&&Trace.readSource==value&&Trace.targets==targets);Trace.hit(8);Trace.nextSize=999;Trace.factor=-1.0f;if(mode==4)throw runtime;if(mode==5)throw error;if(mode==10)ScopedIntegerGetterDelegationHarness.<RuntimeException>fail(checked);}};
            Trace.targetAction=new Runnable(){public void run(){require(Trace.events==18263&&Trace.instance!=null&&Trace.created==selected);Trace.hit(9);Core c=(Core)Trace.instance;require(c.size==size&&Float.floatToRawIntBits(c.factor)==Float.floatToRawIntBits(factor)&&c.factory==maker&&c.created==selected);Trace.payload=payload;Trace.readSource=null;Trace.nextSize=777;}};
            Core<String,Object> result=null;Throwable failure=null;try{result=call(route,operation,factor,value,maker);}catch(Throwable caught){failure=caught;}
            boolean target=mode!=1&&mode!=4&&mode!=5&&mode!=6&&mode!=10,made=target&&mode!=7,created=made&&mode!=8&&mode!=11;
            long events=mode==1?0:mode==6?1:mode==4||mode==5||mode==10?18:mode==7?182:mode==8||mode==11?1826:1826394;
            require(Trace.events==events&&Trace.reads==reads+(mode==1?0:1)&&Trace.targets==targets+(target?1:0)&&Trace.calls==calls+(made?1:0)&&Trace.wrong==wrong);
            require((Trace.instance!=null)==created&&Trace.created==(created?selected:sentinel)&&Trace.payload==(created?payload:sentinel)&&Trace.afterSource==(created?value:other));
            if(target)require(Trace.snapshot==size&&Trace.factory==maker&&Float.floatToRawIntBits(Trace.factor)==Float.floatToRawIntBits(factor));
            if(result!=null)require(result==Trace.instance&&result instanceof Tagged&&result.size==size&&result.created==selected);
            Throwable wanted=mode==4||mode==8?runtime:mode==5?error:mode==6||mode==7||mode==9?Trace.failure:mode==10?checked:null;
            if(wanted!=null)require(failure==wanted&&result==null);
            else if(mode==1)require(failure instanceof NullPointerException&&result==null);
            else if(mode==11)require(failure instanceof ClassCastException&&result==null);
            else require(failure==null&&result!=null);
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
