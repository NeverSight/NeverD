import fixture.Core;
import fixture.Trace;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class SortedEntryMapHarness {
    private static void check(boolean ok,String detail) {
        if(!ok)throw new AssertionError("owned sorted entry map mismatch: "+detail);
    }
    private static void attempt(int route,int mode) {
        Object key0=new Object(),value0=new Object(),key1=new Object(),value1=new Object(),marker=new Object(),lateMarker=new Object();
        RuntimeException runtime=new IllegalStateException("owned runtime");Error error=new LinkageError("owned error");int[] counts=new int[7];
        Map.Entry firstEntry=new Map.Entry() {
            public Object getKey(){counts[4]++;check(counts[4]==1 && counts[3]==1 && Trace.stage==1,"key order");if(mode==12)throw runtime;if(mode==13)throw error;return key0;}
            public Object getValue(){counts[5]++;check(counts[5]==1 && counts[4]==1 && Trace.stage==1,"value order");if(mode==14)throw runtime;if(mode==15)throw error;return value0;}
            public Object setValue(Object value){throw new AssertionError("owned sorted entry map mismatch: unexpected entry mutation");}
        };
        Map.Entry secondEntry=mode==21?firstEntry:new Map.Entry() {
            public Object getKey(){counts[4]++;check(counts[4]==1 && counts[3]==1 && Trace.stage==1,"second key order");return key1;}
            public Object getValue(){counts[5]++;check(counts[5]==1 && counts[4]==1 && Trace.stage==1,"second value order");return value1;}
            public Object setValue(Object value){throw new AssertionError("owned sorted entry map mismatch: unexpected second mutation");}
        };
        Object first=mode==8?new Object():mode==9?null:firstEntry;
        Iterator cursor=new Iterator() {
            public boolean hasNext(){throw new AssertionError("owned sorted entry map mismatch: eager hasNext");}
            public Object next(){counts[2]++;check(counts[2]<=2 && Trace.stage==1,"next order");if(mode==6)throw runtime;if(mode==7)throw error;return counts[2]==1?first:secondEntry;}
        };
        Set actual=new AbstractSet() {
            public int size(){return 2;}
            public Iterator iterator(){counts[1]++;check(counts[1]==1 && Trace.stage==1,"iterator order");return cursor;}
        };
        Collection input=mode==1?null:actual;
        Map view=mode==19?null:new AbstractMap() {
            public Set entrySet(){counts[6]++;check(counts[6]==1 && Trace.calls==0 && Trace.stage==0,"entrySet before provider");return (Set)input;}
        };
        Comparator comparator=mode==2?null:(left,right) -> {
            counts[3]++;check(counts[3]==1 && left==first && right==secondEntry && counts[2]==2 && Trace.stage==1,"exact comparator references and order");
            if(mode==10)throw runtime;if(mode==11)throw error;return mode==20?1:-1;
        };
        LinkedHashMap late=new LinkedHashMap();late.put(key1,value1);
        Trace.calls=Trace.stage=0;Trace.input=null;Trace.comparator=null;Trace.result=new LinkedHashMap();Trace.marker=marker;Trace.selected=Trace.built=null;
        Trace.action=mode==3?null:() -> {
            counts[0]++;check(counts[0]==1 && Trace.calls==1 && Trace.stage==1 && Trace.input==input && Trace.comparator==comparator,"exact provider references");
            if(mode==4)throw runtime;if(mode==5)throw error;
            Iterator it=input.iterator();Map.Entry a=(Map.Entry)it.next(),b=(Map.Entry)it.next();
            Map.Entry selected=((Comparator)comparator).compare(a,b)<=0?a:b;
            Object key=selected.getKey(),value=selected.getValue();LinkedHashMap built=new LinkedHashMap();built.put(key,value);Trace.selected=selected;Trace.built=built;Trace.result=built;
            if(mode==16)Trace.result=null;if(mode==17)Trace.result=new Object();if(mode==18){Trace.result=late;Trace.marker=lateMarker;}
        };
        Object result=null;Throwable failure=null;
        try {
            switch(route) {
                case 0:result=Core.direct(input,comparator);break;
                case 1:result=Core.ignored(input,comparator);break;
                case 2:result=Core.relay(input,comparator);break;
                case 3:result=Core.fromMap(view,comparator);break;
                default:throw new AssertionError("owned route");
            }
        }catch(Throwable caught){failure=caught;}
        boolean early=route==3 && mode==19;
        int action=early || mode==3?0:1;
        int iter=action==0 || mode==1 || mode==4 || mode==5?0:1;
        int next=iter==0?0:mode==6 || mode==7 || mode==8?1:2;
        int compare=next!=2 || mode==2?0:1;
        int keys=compare==0 || mode==9 || mode==10 || mode==11?0:1;
        int values=keys==0 || mode==12 || mode==13?0:1;
        boolean actionFailed=mode<=15 && mode!=0;
        check(Trace.calls==(early?0:1) && Trace.input==(early?null:input) && Trace.comparator==(early?null:comparator)
                && Trace.stage==(early?0:actionFailed?1:2) && counts[0]==action && counts[1]==iter && counts[2]==next && counts[3]==compare && counts[4]==keys && counts[5]==values && counts[6]==(route==3 && !early?1:0),"exact original partial effects");
        if(early || mode==1 || mode==2 || mode==3 || mode==9)check(failure instanceof NullPointerException,"original null failure");
        else if(mode==4 || mode==6 || mode==10 || mode==12 || mode==14)check(failure==runtime,"runtime identity");
        else if(mode==5 || mode==7 || mode==11 || mode==13 || mode==15)check(failure==error,"error identity");
        else if(mode==8 || mode==17)check(failure instanceof ClassCastException,"original entry or provider cast");
        else check(failure==null && result==(route==1?mode==18?lateMarker:marker:mode==16?null:mode==18?late:Trace.built),"opaque result and late marker identity");
        if(!early && (mode==0 || mode>=16)) {
            Object expectedKey=mode==20?key1:key0,expectedValue=mode==20?value1:value0;
            check(Trace.selected==(mode==20?secondEntry:firstEntry) && Trace.built instanceof LinkedHashMap && ((Map)Trace.built).size()==1 && ((Map)Trace.built).get(expectedKey)==expectedValue,"selected entry and exact map contents");
        }
    }
    public static void main(String[] args) {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        for(int i=0;i<2;i++)attempt(route,mode);
        System.out.println("attempts=2,sorted-entry-map=matched");
    }
}
