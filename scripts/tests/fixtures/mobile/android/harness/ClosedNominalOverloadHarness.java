import fixture.*;
import kotlin.jvm.functions.Function2;
public class ClosedNominalOverloadHarness {
    static void require(boolean value){if(!value)throw new AssertionError("owned closed nominal overload");}
    public static void main(String[]args)throws Throwable {
        int route=Integer.parseInt(args[0]),kind=Integer.parseInt(args[1]),mode=Integer.parseInt(args[2]);
        int[] masks={0,1,2,3,4,5,6,7,8,Integer.MAX_VALUE};
        for(int attempt=0;attempt<2;attempt++) {
            Trace.events=0;Trace.factoryCalls=0;Trace.unexpected=0;Trace.partial=null;Trace.legacy=null;Trace.firstSeen=null;Trace.secondSeen=null;Trace.restSeen=null;Trace.handlerSeen=null;
            int fail=mode>=10?(mode-10)%5+1:0;
            Throwable requestedFailure=mode>=20?new AssertionError("owned error"):new IllegalStateException("owned runtime");
            Trace.fail=fail;Trace.failure=requestedFailure;
            int mask=masks[mode%10]^(attempt==1?Integer.MIN_VALUE:0);Object shared=new Object();
            Object first=mode%4==0?null:shared,second=mode%4==1?null:mode%4==2?first:new Object();
            Object[] rest=mode%3==0?null:attempt==0?new String[]{"owned",null}:new Object[]{first,second,shared};
            ClosedNominalOverloadProbe dual=attempt==0?new ClosedNominalOverloadProbe():new ClosedNominalOverloadProbe.Child();
            Handler handler=mode%3==0?null:dual,selected=mode%3==2?null:new ClosedNominalOverloadProbe.Child();
            Handler replacement=new ClosedNominalOverloadProbe();Function2 block=mode%3==1?null:dual;
            Trace.selected=selected;Trace.replacement=replacement;Core result=null;Throwable failure=null;
            try{result=ClosedNominalOverloadCaller.make(route,kind,first,second,rest,handler,block,mask);}catch(Throwable error){failure=error;}
            long events=0;boolean partial=false,captured=false,legacy=false,done=false,factory=false;Throwable expectedFailure=null;
            int[] steps=kind==0?new int[]{1,2,3}:new int[]{4,1,2,3,5};
            for(int step:steps) {
                if(step==4)factory=true;if(step==1)partial=true;if(step==2)captured=true;if(step==5)legacy=true;
                events=events*10+step;
                if(step==fail){expectedFailure=requestedFailure;break;}
                if(step==(kind==0?3:5))done=true;
            }
            Object expectedFirst=kind==0&&(mask&1)!=0?null:first,expectedSecond=kind==0&&(mask&2)!=0?null:second;
            Object[] expectedRest=kind==0&&(mask&4)!=0?null:rest;Handler expectedHandler=kind==0?handler:selected;
            require(Trace.fail==fail&&Trace.failure==requestedFailure&&failure==expectedFailure&&Trace.events==events&&Trace.factoryCalls==(factory?1:0)&&Trace.unexpected==0);
            require((Trace.partial!=null)==partial&&(Trace.legacy!=null)==legacy&&Trace.selected==(captured?replacement:selected)&&Trace.replacement==replacement);
            require(Trace.firstSeen==(captured?expectedFirst:null)&&Trace.secondSeen==(captured?expectedSecond:null)&&Trace.restSeen==(captured?expectedRest:null)&&Trace.handlerSeen==(captured?expectedHandler:null));
            if(partial) {
                Core value=(Core)Trace.partial;
                require(value.first==expectedFirst&&value.second==expectedSecond&&value.rest==expectedRest&&value.handler==expectedHandler);
                require(value.legacyBlock==(legacy?block:null)&&Trace.legacy==(legacy?value:null)&&result==(done?value:null));
            }else require(result==null);
            System.out.println("route="+route+",kind="+kind+",mode="+mode+",attempt="+attempt+",events="+Trace.events+",mask="+mask);
        }
    }
}
