import fixture.Carrier;
import fixture.Core;
import fixture.Trace;
import java.util.*;
@SuppressWarnings({"rawtypes","unchecked"})
public final class EnumSetTokenFactoryHarness {
    enum Tone {A {public int value(){return 1;}},B;public int value(){return 2;}}
    enum Other {C}
    enum Empty {}
    private static void check(boolean ok,String detail) {
        if(!ok)throw new AssertionError("owned enum set token factory mismatch: "+detail);
    }
    private static void attempt(int route,int mode) {
        Class token=mode==1?Other.class:mode==2?null:mode==3?String.class:mode==4?int.class:
                mode==5?Tone[].class:mode==6?Tone.A.getClass():mode==7?Empty.class:Tone.class;
        Object argument=route==2 && mode==15?new Object():token;
        Object marker=new Object(),lateMarker=new Object();Carrier original=new Carrier(marker),late=new Carrier(lateMarker);
        RuntimeException runtime=new IllegalStateException("owned runtime");Error error=new LinkageError("owned error");int[] calls={0};
        Trace.calls=Trace.stage=0;Trace.token=null;Trace.result=original;Trace.marker=marker;
        Trace.action=mode==10?null:() -> {
            calls[0]++;check(calls[0]==1 && Trace.calls==1 && Trace.stage==1 && Trace.token==token,"exact token and action order");
            if(mode==8)throw runtime;if(mode==9)throw error;if(mode==11)Trace.result=late;
            if(mode==12)Trace.result=new Object();if(mode==13)Trace.result=null;if(mode==14)Trace.marker=lateMarker;
        };
        Object result=null;Throwable failure=null;
        try {
            switch(route) {
                case 0:result=Core.direct(token);break;
                case 1:result=Core.ignored(token);break;
                case 2:result=Core.checked(argument);break;
                case 3:result=Core.relay(token);break;
                case 4:result=Core.empty(token);break;
                case 5:result=Core.unknown(token);break;
                default:throw new AssertionError("owned route");
            }
        } catch(Throwable caught){failure=caught;}
        if(route>=4) {
            check(Trace.calls==0 && Trace.stage==0 && calls[0]==0,"JDK call keeps provider untouched");
            if(mode==2)check(failure instanceof NullPointerException,"JDK null token failure");
            else if(mode>=3 && mode<=6)check(failure instanceof ClassCastException,"JDK non-enum token failure");
            else {
                check(failure==null && result instanceof EnumSet && ((EnumSet)result).isEmpty(),"original empty enum set");
                EnumSet values=(EnumSet)result;
                if(token!=Empty.class) {
                    Object value=token==Other.class?Other.C:Tone.A;
                    boolean added=false;Throwable insertionFailure=null;
                    try {added=values.add(value);}catch(Throwable caught){insertionFailure=caught;}
                    check(insertionFailure==null && added && values.contains(value) && values.size()==1,"exact accepted enum token");
                    Object wrong=token==Other.class?Tone.B:Other.C;Throwable rejected=null;
                    try {values.add(wrong);}catch(Throwable caught){rejected=caught;}
                    check(rejected instanceof ClassCastException && values.size()==1,"other enum rejected after original set creation");
                }
            }
            return;
        }
        if(route==2 && mode==15) {
            check(failure instanceof ClassCastException && Trace.calls==0 && Trace.stage==0 && calls[0]==0,"argument cast precedes provider");return;
        }
        check(Trace.calls==1 && Trace.token==token && Trace.stage==(mode>=8 && mode<=10?1:2) && calls[0]==(mode==10?0:1),"exact opaque provider effects");
        if(mode==8)check(failure==runtime,"runtime identity");
        else if(mode==9)check(failure==error,"error identity");
        else if(mode==10)check(failure instanceof NullPointerException,"original action null failure");
        else if(mode==12)check(failure instanceof ClassCastException,"original returned carrier cast");
        else check(failure==null && result==(route==1?mode==14?lateMarker:marker:mode==11?late:mode==13?null:original),"original late result reference");
    }
    public static void main(String[] args) {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        for(int i=0;i<2;i++)attempt(route,mode);
        System.out.println("attempts=2,enum-set-token-factory=matched");
    }
}
