import fixture.*;
import java.io.IOException;
import java.util.Map;
import java.util.Collection;
import java.util.ArrayList;
import java.util.HashMap;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class SyntheticNullaryAllocationHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned synthetic nullary allocation");}
    @SuppressWarnings("unchecked") private static <X extends Throwable> void fail(Throwable failure)throws X{throw (X)failure;}
    @SuppressWarnings("unchecked") private static Core<String,Object> call(int route,int operation,float factor,Map<String,Collection<Object>> value)throws Throwable {
        if(route==0)return operation==0?new Core<String,Object>(factor,value):new Core<String,Object>(value,factor);
        if(route==1)return operation==0?SyntheticNullaryAllocationCaller.first(factor,value):SyntheticNullaryAllocationCaller.second(factor,value);
        if(route==2){try{return (Core)(operation==0?Core.class.getConstructor(float.class,Map.class).newInstance(factor,value):Core.class.getConstructor(Map.class,float.class).newInstance(value,factor));}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,float.class,Map.class)).invokeExact(factor,value);
        return (Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Map.class,float.class)).invokeExact(value,factor);
    }
    @SuppressWarnings({"rawtypes","unchecked"}) public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final RuntimeException runtime=new IllegalArgumentException("owned failure");final Error error=new Error("owned error");final IOException checked=new IOException("owned checked");Trace.failure=new IllegalArgumentException("owned stage");
        Trace.failAt=mode==6?1:mode==7?2:mode==8?3:mode==9?4:0;
        final float factor=mode==2?Float.intBitsToFloat(0x7fc01234):0.625f;
        for(int attempt=0;attempt<2;attempt++) {
            final Object sentinel=new Object(),payload=new Object();final Collection<Object> selected=new ArrayList<Object>(),other=new ArrayList<Object>();
            final Object expected=mode==3?null:mode==11?Integer.valueOf(37):selected;
            final Map<String,Collection<Object>> value=mode==1?null:new HashMap<String,Collection<Object>>();
            Trace.events=0;Trace.adapter=null;Trace.factory=null;Trace.map=null;Trace.factor=0;Trace.created=sentinel;Trace.instance=null;Trace.payload=sentinel;Trace.result=expected;
            final int adapters=Trace.adapters,targets=Trace.targets,calls=Trace.calls,wrong=Trace.wrong;
            Trace.allocateAction=new Runnable(){public void run(){require(Trace.events==1&&Trace.adapter!=null&&Trace.factory==null);Trace.hit(8);if(mode==4)throw runtime;if(mode==5)throw error;}};
            Trace.makeAction=new Runnable(){public void run(){require(Trace.events==1826&&Trace.factory==Trace.adapter&&Trace.map==value&&Float.floatToRawIntBits(Trace.factor)==Float.floatToRawIntBits(factor));Trace.hit(7);Trace.result=other;if(mode==10)SyntheticNullaryAllocationHarness.<RuntimeException>fail(checked);}};
            Trace.targetAction=new Runnable(){public void run(){require(Trace.events==182673&&Trace.instance!=null&&Trace.created==expected&&Trace.result==other);Trace.hit(9);Core c=(Core)Trace.instance;require(c.original==value&&c.factory==Trace.adapter&&c.created==expected&&Float.floatToRawIntBits(c.factor)==Float.floatToRawIntBits(factor));Trace.payload=payload;if(value!=null)value.put("observed",selected);}};
            Core<String,Object> result=null;Throwable failure=null;try{result=call(route,operation,factor,value);}catch(Throwable caught){failure=caught;}
            boolean target=mode!=4&&mode!=5&&mode!=6,made=target&&mode!=7,created=made&&mode!=10&&mode!=11,stored=created&&mode!=8,returned=stored&&mode!=9;
            long events=mode==6?1:mode==4||mode==5?18:mode==7?182:mode==10||mode==11?18267:mode==8?182673:mode==9?18267394:182673945;
            require(Trace.adapters==adapters+1&&Trace.targets==targets+(target?1:0)&&Trace.calls==calls+(made?1:0)&&Trace.wrong==wrong&&Trace.events==events);
            require((Trace.instance!=null)==stored&&(result!=null)==returned&&Trace.created==(created?expected:sentinel)&&Trace.payload==(stored?payload:sentinel));
            if(target)require(Trace.map==value&&Trace.factory==Trace.adapter&&Float.floatToRawIntBits(Trace.factor)==Float.floatToRawIntBits(factor));
            if(returned)require(result==Trace.instance&&result instanceof Tagged&&result.created==expected);
            if(value!=null)require(value.containsKey("observed")==stored);
            Throwable wanted=mode==4?runtime:mode==5?error:mode>=6&&mode<=9?Trace.failure:mode==10?checked:null;
            if(wanted!=null)require(failure==wanted&&result==null);
            else if(mode==11)require(failure instanceof ClassCastException&&result==null);
            else require(failure==null&&result!=null);
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
