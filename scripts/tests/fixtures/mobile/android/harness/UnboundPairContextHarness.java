import fixture.Core;
import fixture.Trace;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class UnboundPairContextHarness {
    private static void check(boolean ok,String detail) {
        if(!ok)throw new AssertionError("owned unbound pair context mismatch: "+detail);
    }
    private static void attempt(final int route,final int mode) {
        final String left=mode==15?null:new String("context"),right=mode==16?null:mode==17?left:new String("context");
        final Object element=mode==14?null:new Object(),marker=new Object(),lateMarker=new Object();
        final Map original=new HashMap(),late=new HashMap(),tree=new TreeMap();final Object invalid=new Object();
        final RuntimeException runtime=new IllegalStateException("owned runtime");final Error error=new LinkageError("owned error");
        final int[] counts=new int[4];final Object[] seen={null};
        final Iterator actual=new Iterator() {
            public boolean hasNext() {counts[2]++;check(counts[2]==1 && Trace.stage==1,"hasNext order");if(mode==6)throw runtime;if(mode==7)throw error;return mode!=8;}
            public Object next() {counts[3]++;check(counts[2]==1 && counts[3]==1 && Trace.stage==1,"next order");if(mode==9)throw runtime;if(mode==10)throw error;return element;}
        };
        final Iterator cursor=mode==2?null:actual;
        final Iterable source=mode==2?null:new Iterable() {
            public Iterator iterator() {counts[0]++;check(counts[0]==1 && Trace.calls==0,"iterator before provider");if(mode==15)throw runtime;if(mode==16)throw error;return cursor;}
        };
        Trace.calls=Trace.stage=0;Trace.cursor=null;Trace.left=Trace.right=null;Trace.result=mode==11?tree:original;Trace.marker=marker;
        Trace.action=mode==3?null:() -> {
            counts[1]++;check(counts[1]==1 && Trace.calls==1 && Trace.stage==1 && Trace.cursor==cursor && Trace.left==left && Trace.right==right,"exact iterator and provider action");
            if(mode==4)throw runtime;if(mode==5)throw error;
            if(mode!=1 && cursor.hasNext())seen[0]=cursor.next();
            if(mode==12)Trace.result=late;if(mode==13)Trace.result=invalid;if(mode==17)Trace.result=null;
            if(mode==18)Trace.marker=lateMarker;if(mode==19)Trace.cursor=null;
        };
        Object result=null;Throwable failure=null;
        try {
            switch(route) {
                case 0:result=Core.direct(cursor,left,right);break;
                case 1:result=Core.from(source,left,right);break;
                case 2:result=Core.checked(cursor,left,right);break;
                case 3:result=Core.ignored(cursor,left,right);break;
                case 4:result=Core.relay(cursor,left,right);break;
                default:throw new AssertionError("owned route");
            }
        } catch(Throwable caught){failure=caught;}
        boolean bridgeFailure=route==1 && (mode==15 || mode==16);
        if(bridgeFailure) {
            check(failure==(mode==15?runtime:error) && counts[0]==1 && Trace.calls==0 && counts[1]==0 && Trace.stage==0,"bridge failure precedes provider");return;
        }
        int has=mode==1 || mode==2 || mode==3 || mode==4 || mode==5?0:1;
        int next=has==0 || mode==6 || mode==7 || mode==8?0:1;
        boolean actionFailed=mode==2 || mode==3 || mode==4 || mode==5 || mode==6 || mode==7 || mode==9 || mode==10;
        check(counts[0]==(route==1 && mode!=2?1:0) && Trace.calls==1 && Trace.stage==(actionFailed?1:2)
                && counts[1]==(mode==3?0:1) && counts[2]==has && counts[3]==next,"exact bridge, iterator and partial effects");
        if(mode==4 || mode==6 || mode==9)check(failure==runtime,"runtime identity");
        else if(mode==5 || mode==7 || mode==10)check(failure==error,"error identity");
        else if(mode==2 || mode==3)check(failure instanceof NullPointerException,"original null failure");
        else if(mode==13 || mode==11 && route==2)check(failure instanceof ClassCastException,"original delayed map cast");
        else check(failure==null && result==(route==3?mode==18?lateMarker:marker:mode==12?late:mode==17?null:mode==11?tree:original)
                && seen[0]==(next==1?element:null),"opaque result and exact element reference");
    }
    public static void main(String[] args) {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        for(int i=0;i<2;i++)attempt(route,mode);
        System.out.println("attempts=2,unbound-pair-context=matched");
    }
}
