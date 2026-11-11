import fixture.Core;
import fixture.Trace;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class PairedArrayMapHarness {
    private static void check(boolean ok,String detail) {
        if(!ok)throw new AssertionError("owned paired array map mismatch: "+detail);
    }
    private static void attempt(int route,int mode) {
        Object key=mode==15?null:new String("owned key"),value=mode==16?null:new String("owned value");
        Object marker=new Object(),lateMarker=new Object();boolean flag=(mode&1)!=0;
        RuntimeException runtime=new IllegalStateException("owned runtime");Error error=new LinkageError("owned error");int[] counts=new int[1];
        Object[] first=(route==3 || mode==10)?new String[]{(String)key,"spare"}:new Object[]{key,new Object()};
        Object[] second=(route==3 || mode==19)?new String[]{(String)value,"spare"}:new Object[]{value,new Object()};
        if(mode==3)first=route==3?new String[0]:new Object[0];if(mode==4)second=route==3?new String[0]:new Object[0];if(mode==18)second=first;
        Object[] keys=mode==1?null:first,values=mode==2?null:second;
        Object beforeKey=keys!=null && keys.length!=0?keys[0]:null,beforeValue=values!=null && values.length!=0?values[0]:null;
        Object replacement=mode==10?new Object():new String("owned replacement");Map late=new IdentityHashMap();
        Trace.calls=Trace.stage=Trace.step=0;Trace.keys=Trace.values=null;Trace.flag=!flag;Trace.result=new HashMap();Trace.marker=marker;Trace.built=null;
        Trace.action=mode==5?null:() -> {
            counts[0]++;check(counts[0]==1 && Trace.calls==1 && Trace.stage==1 && Trace.keys==keys && Trace.values==values && Trace.flag==flag,"exact array/flag references and provider order");
            if(mode==6)throw runtime;if(mode==7)throw error;
            Object selectedKey=keys[0];Trace.step=1;if(mode==8)throw runtime;if(mode==9)throw error;
            Object selectedValue=values[0];Trace.step=2;keys[0]=replacement;Trace.step=3;
            if(mode==11)throw runtime;if(mode==12)throw error;
            Map built=new HashMap();built.put(selectedKey,selectedValue);Trace.built=built;Trace.result=built;
            if(mode==13)Trace.result=null;if(mode==14)Trace.result=new Object();if(mode==17){late.putAll(built);Trace.result=late;Trace.marker=lateMarker;}
        };
        Object result=null;Throwable failure=null;
        try {
            switch(route) {
                case 0:result=Core.direct(keys,values,flag);break;
                case 1:result=Core.relay(keys,values,flag);break;
                case 2:result=Core.ignored(keys,values,flag);break;
                case 3:result=Core.strings((String[])keys,(String[])values,flag);break;
                default:throw new AssertionError("owned route");
            }
        }catch(Throwable caught){failure=caught;}
        int step=mode==1 || mode==3 || mode==5 || mode==6 || mode==7?0:mode==2 || mode==4 || mode==8 || mode==9?1:mode==10?2:3;
        boolean actionFailed=mode>=1 && mode<=12;
        check(Trace.calls==1 && Trace.keys==keys && Trace.values==values && Trace.flag==flag && Trace.stage==(actionFailed?1:2) && Trace.step==step && counts[0]==(mode==5?0:1),"exact original partial array effects");
        if(keys!=null && keys.length!=0)check(keys[0]==(step==3?replacement:beforeKey),"original key-array write timing");
        if(values!=null && values.length!=0)check(values[0]==(keys==values && step==3?replacement:beforeValue),"distinct or aliased value-array effects");
        if(mode==1 || mode==2 || mode==5)check(failure instanceof NullPointerException,"original null failure");
        else if(mode==3 || mode==4)check(failure instanceof ArrayIndexOutOfBoundsException,"original bound failure");
        else if(mode==6 || mode==8 || mode==11)check(failure==runtime,"runtime identity");
        else if(mode==7 || mode==9 || mode==12)check(failure==error,"error identity");
        else if(mode==10)check(failure instanceof ArrayStoreException,"original covariant store failure");
        else if(mode==14)check(failure instanceof ClassCastException,"original provider result cast");
        else check(failure==null && result==(route==2?mode==17?lateMarker:marker:mode==13?null:mode==17?late:Trace.built),"opaque returned map and late marker identity");
        if(!actionFailed)check(Trace.built instanceof Map && ((Map)Trace.built).size()==1 && ((Map)Trace.built).get(beforeKey)==beforeValue,"original selected component identities");
    }
    public static void main(String[] args) {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        for(int i=0;i<2;i++)attempt(route,mode);
        System.out.println("attempts=2,paired-array-map=matched");
    }
}
