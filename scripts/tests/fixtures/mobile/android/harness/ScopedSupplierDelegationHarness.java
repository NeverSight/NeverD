import fixture.*;
import java.util.Map;
import java.util.HashMap;
import java.util.function.Supplier;
import java.util.function.Function;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.lang.reflect.InvocationTargetException;
public final class ScopedSupplierDelegationHarness {
    private static void require(boolean ok){if(!ok)throw new AssertionError("owned scoped supplier delegation");}
    @SuppressWarnings({"unchecked","rawtypes"})
    private static Core<String,Object> call(int route,int operation,Supplier<Map<String,Object>> source,Function<Object,String> keys,Function<Object,Object> values)throws Throwable {
        if(route==0)return operation==0?new Core<String,Object>(source,keys,values):new Core<String,Object>(source,keys,values,7);
        if(route==1)return operation==0?ScopedSupplierDelegationCaller.first(source,keys,values):ScopedSupplierDelegationCaller.second(source,keys,values);
        if(route==2){try{return operation==0?Core.class.getConstructor(Supplier.class,Function.class,Function.class).newInstance(source,keys,values):Core.class.getConstructor(Supplier.class,Function.class,Function.class,int.class).newInstance(source,keys,values,7);}catch(InvocationTargetException failure){throw failure.getCause();}}
        if(operation==0)return (Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Supplier.class,Function.class,Function.class)).invokeExact(source,keys,values);
        return (Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,Supplier.class,Function.class,Function.class,int.class)).invokeExact(source,keys,values,7);
    }
    @SuppressWarnings({"unchecked","rawtypes"})
    public static void main(String[] args)throws Throwable {
        final int route=Integer.parseInt(args[0]),operation=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        final RuntimeException runtime=new IllegalArgumentException("owned runtime");final Error error=new Error("owned error");
        Trace.failure=new IllegalArgumentException("owned stage");Trace.failAt=mode==8?4:0;
        for(int attempt=0;attempt<2;attempt++) {
            final Map<String,Object> map=new HashMap<String,Object>(),other=new HashMap<String,Object>();
            final Object first=new Object(),second=new Object(),sentinel=new Object(),got=new Object(),parented=new Object(),constructed=new Object(),input=new Object();
            map.put("a",first);map.put("b",second);Trace.events=0;Trace.parent=null;Trace.core=null;Trace.input=input;Trace.provided=mode==4?new Object():mode==5?null:map;
            final Object supplied=Trace.provided;Trace.getPayload=sentinel;Trace.parentPayload=sentinel;Trace.constructorPayload=sentinel;
            final int gets=Trace.gets,parents=Trace.parents,constructors=Trace.constructors,keyCalls=Trace.keyCalls,valueCalls=Trace.valueCalls,wrong=Trace.wrong;
            final Object key=mode==9?Integer.valueOf(1):mode==10?null:new String("owned-key");
            final Supplier source=mode==1?null:new Supplier(){public Object get(){Object saved=Trace.provided;Trace.gets++;Trace.hit(1);Trace.getAction.run();return saved;}};
            final Function keys=new Function(){public Object apply(Object arg){Trace.keyCalls++;require(arg==input&&Trace.events==1234&&Trace.core!=null&&Trace.core.keys==this);Trace.hit(5);Trace.dynamicKey=other;return key;}};
            final Function values=new Function(){public Object apply(Object arg){Trace.valueCalls++;require(arg==input&&Trace.events==12345&&Trace.core!=null&&Trace.core.values==this);Trace.hit(6);Trace.dynamicValue=other;if(mode==11)throw runtime;return second;}};
            Trace.dynamicSource=source;Trace.dynamicKey=keys;Trace.dynamicValue=values;
            Trace.getAction=new Runnable(){public void run(){require(Trace.events==1&&Trace.parent==null&&Trace.core==null&&Trace.dynamicSource==source);Trace.getPayload=got;Trace.provided=other;Trace.dynamicSource=other;if(mode==2)throw runtime;if(mode==3)throw error;}};
            Trace.parentAction=new Runnable(){public void run(){require(Trace.events==12&&Trace.parent.map==supplied&&Trace.core==null&&Trace.provided==other);Trace.parentPayload=parented;if(supplied!=null)map.put("parent",parented);if(mode==6)throw runtime;}};
            Trace.constructorAction=new Runnable(){public void run(){require(Trace.events==123&&Trace.core.map==supplied&&Trace.parent==Trace.core&&Trace.core.keys==keys&&Trace.core.values==values);Trace.constructorPayload=constructed;if(supplied!=null)map.put("core",constructed);if(mode==7)throw error;}};
            Core<String,Object> result=null;String actualKey=null;Object actualValue=null;Throwable failure=null;
            try{result=call(route,operation,source,keys,values);actualKey=result.readKey();require(actualKey.length()==9);actualValue=result.readValue();}catch(Throwable caught){failure=caught;}
            boolean getReached=mode!=1,parentReached=getReached&&mode!=2&&mode!=3&&mode!=4,constructorReached=parentReached&&mode!=6,wrapperReached=constructorReached&&mode!=7,keyReached=wrapperReached&&mode!=8,valueReached=keyReached&&mode!=9&&mode!=10;
            long events=mode==1?0:!parentReached?1:!constructorReached?12:!wrapperReached?123:!keyReached?1234:!valueReached?12345:123456;
            require(Trace.events==events&&Trace.gets==gets+(getReached?1:0)&&Trace.parents==parents+(parentReached?1:0)&&Trace.constructors==constructors+(constructorReached?1:0)&&Trace.keyCalls==keyCalls+(keyReached?1:0)&&Trace.valueCalls==valueCalls+(valueReached?1:0)&&Trace.wrong==wrong);
            Throwable expected=mode==2||mode==6||mode==11?runtime:mode==3||mode==7?error:mode==8?Trace.failure:null;
            if(mode==1)require(failure instanceof NullPointerException&&result==null);
            else if(mode==4)require(failure instanceof ClassCastException&&result==null);
            else if(expected!=null)require(failure==expected&&(result!=null)==(mode==11));
            else if(mode==9)require(failure instanceof ClassCastException&&result!=null);
            else if(mode==10)require(failure instanceof NullPointerException&&result!=null&&actualKey==null);
            else require(failure==null&&result!=null&&actualKey==key&&actualValue==second);
            require(Trace.getPayload==(getReached?got:sentinel)&&Trace.parentPayload==(parentReached?parented:sentinel)&&Trace.constructorPayload==(constructorReached?constructed:sentinel));
            require((Trace.parent!=null)==parentReached&&(Trace.core!=null)==constructorReached);
            if(parentReached)require(Trace.parent.map==supplied);
            if(constructorReached)require(Trace.core.map==supplied&&Trace.core.keys==keys&&Trace.core.values==values);
            require(map.get("a")==first&&map.get("b")==second&&map.size()==2+(parentReached&&mode!=5?1:0)+(constructorReached&&mode!=5?1:0));
            System.out.println("route="+route+",operation="+operation+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
