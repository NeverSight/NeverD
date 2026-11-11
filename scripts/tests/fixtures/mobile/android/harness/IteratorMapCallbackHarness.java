import fixture.Core;
import fixture.Mapper;
import fixture.Trace;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class IteratorMapCallbackHarness {
    private static void check(boolean ok,String detail) {
        if(!ok)throw new AssertionError("owned iterator map callback mismatch: "+detail);
    }
    private static <T extends Throwable> Object escape(Throwable failure) throws T {throw (T)failure;}
    private static Object apply(Mapper callback,Object input) {
        try{return callback.apply(input);}catch(RuntimeException failure){throw failure;}catch(Exception failure){return IteratorMapCallbackHarness.<RuntimeException>escape(failure);}
    }
    private static void attempt(int route,int mode) {
        boolean mapped=route>=3;Object element=mode==19?null:new Object(),selectedKey=new Object(),selectedValue=mapped?new Object():element,marker=new Object(),lateMarker=new Object();
        RuntimeException runtime=new IllegalStateException("owned runtime");Error error=new LinkageError("owned error");Exception checked=new Exception("owned checked");int[] counts=new int[5];
        Iterator actual=new Iterator() {
            public boolean hasNext(){throw new AssertionError("owned iterator map callback mismatch: eager hasNext");}
            public Object next(){counts[1]++;check(counts[1]==1 && Trace.stage==1,"next order");if(mode==8)throw runtime;if(mode==9)throw error;return element;}
        };
        Iterator input=mode==1?null:actual;
        Mapper key=mode==3?null:item -> {
            counts[2]++;check(counts[2]==1 && item==element && counts[1]==1 && Trace.stage==1,"key callback reference/order");
            if(mode==10)throw runtime;if(mode==11)throw error;if(mode==20)throw checked;return selectedKey;
        };
        Mapper value=mode==4?null:item -> {
            counts[3]++;check(mapped && counts[3]==1 && item==element && counts[2]==1 && Trace.stage==1,"value callback reference/order");
            if(mode==12)throw runtime;if(mode==13)throw error;if(mode==21)throw checked;return selectedValue;
        };
        Map actualTarget=new HashMap() {
            public Object put(Object key,Object value){counts[4]++;check(counts[4]==1 && key==selectedKey && value==selectedValue && counts[2]==1 && counts[3]==(mapped?1:0) && Trace.stage==1,"exact target/key/value and mutation order");if(mode==14)throw runtime;if(mode==15)throw error;return super.put(key,value);}
        };
        Map target=mode==2?null:actualTarget;
        Map late=new IdentityHashMap();
        Trace.calls=Trace.stage=0;Trace.input=null;Trace.target=null;Trace.key=Trace.value=null;Trace.result=new HashMap();Trace.marker=marker;
        Trace.action=mode==5?null:() -> {
            counts[0]++;check(counts[0]==1 && Trace.calls==1 && Trace.stage==1 && Trace.input==input && Trace.target==target && Trace.key==key && Trace.value==(mapped?value:null),"exact distinct provider references");
            if(mode==6)throw runtime;if(mode==7)throw error;
            Object item=input.next(),keyResult=apply(key,item),valueResult=mapped?apply(value,item):item;target.put(keyResult,valueResult);Trace.result=target;
            if(mode==16)Trace.result=null;if(mode==17)Trace.result=new Object();if(mode==18){late.putAll(target);Trace.result=late;Trace.marker=lateMarker;}
        };
        Object result=null;Throwable failure=null;
        try {
            switch(route) {
                case 0:result=Core.directKey(input,target,key);break;
                case 1:result=Core.relayKey(input,target,key);break;
                case 2:result=Core.ignoredKey(input,target,key);break;
                case 3:result=Core.directBoth(input,target,key,value);break;
                case 4:result=Core.relayBoth(input,target,key,value);break;
                case 5:result=Core.ignoredBoth(input,target,key,value);break;
                default:throw new AssertionError("owned route");
            }
        }catch(Throwable caught){failure=caught;}
        int action=mode==5?0:1;
        int next=action==0 || mode==1 || mode==6 || mode==7?0:1;
        int keys=next==0 || mode==3 || mode==8 || mode==9?0:1;
        boolean keyFailed=mode==10 || mode==11 || mode==20;
        int values=!mapped || keys==0 || keyFailed || mode==4?0:1;
        boolean valueFailed=mapped && (mode==4 || mode==12 || mode==13 || mode==21);
        int puts=keys==0 || keyFailed || valueFailed || mode==2?0:1;
        boolean actionFailed=mode==1 || mode==2 || mode==3 || mode==5 || mode==6 || mode==7 || mode==8 || mode==9 || keyFailed || valueFailed || mode==14 || mode==15;
        check(Trace.calls==1 && Trace.input==input && Trace.target==target && Trace.key==key && Trace.value==(mapped?value:null) && Trace.stage==(actionFailed?1:2)
                && counts[0]==action && counts[1]==next && counts[2]==keys && counts[3]==values && counts[4]==puts,"exact original partial effects");
        if(mode==1 || mode==2 || mode==3 || mode==5 || mapped && mode==4)check(failure instanceof NullPointerException,"original null failure");
        else if(mode==6 || mode==8 || mode==10 || mapped && mode==12 || mode==14)check(failure==runtime,"runtime identity");
        else if(mode==7 || mode==9 || mode==11 || mapped && mode==13 || mode==15)check(failure==error,"error identity");
        else if(mode==20 || mapped && mode==21)check(failure==checked,"checked callback identity");
        else if(mode==17)check(failure instanceof ClassCastException,"original provider map cast");
        else check(failure==null && result==((route==2 || route==5)?mode==18?lateMarker:marker:mode==16?null:mode==18?late:target),"opaque returned map and late marker identity");
        if(!actionFailed)check(target.size()==1 && target.get(selectedKey)==selectedValue,"original map mutation and value identity");
    }
    public static void main(String[] args) {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        for(int i=0;i<2;i++)attempt(route,mode);
        System.out.println("attempts=2,iterator-map-callback=matched");
    }
}
