import fixture.*;
public class NominalSuperConstructorHarness {
    static void require(boolean ok){if(!ok)throw new AssertionError("owned nominal super constructor");}
    public static void main(String[]args)throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]),stage=mode/4;
        long[] stamps={Long.MIN_VALUE,Long.MAX_VALUE,-1L,0L};
        for(int attempt=0;attempt<2;attempt++) {
            Trace.parentPartial=null;Trace.corePartial=null;Trace.seen=null;Trace.saved=null;Trace.stampSeen=0;Trace.events=0;
            Trace.fail=stage==0?0:stage<=4?stage:stage-4;
            Trace.failure=stage<=4?new IllegalStateException("owned runtime"):new AssertionError("owned error");
            Object payload=attempt==0?new Object():null;
            Token value;switch((mode+attempt)%4){
                case 0:value=new Token(payload);break;
                case 1:value=new NominalSuperConstructorProbe(payload);break;
                case 2:value=new NominalSuperConstructorProbe.Child(payload);break;
                default:value=null;
            }
            long stamp=stamps[(stage+attempt)%4];Core result=null;Throwable failure=null;
            try{result=NominalSuperConstructorCaller.make(route,value,stamp);}catch(Throwable error){failure=error;}
            int end=Trace.fail==0?4:Trace.fail;long events=0;for(int i=1;i<=end;i++)events=events*10+i;
            require(Trace.events==events&&failure==(Trace.fail==0?null:Trace.failure));
            require(Trace.parentPartial instanceof Core);
            Parent<?> parent=(Parent<?>)Trace.parentPartial;Core partial=(Core)Trace.parentPartial;
            boolean parentAssigned=Trace.fail!=1,childAssigned=end==4;
            require(parent.value==(parentAssigned?value:null)&&parent.getValue()==parent.value&&parent.stamp==(parentAssigned?stamp:0));
            require(Trace.seen==(parentAssigned?value:null));
            require(partial.saved==(childAssigned?value:null)&&partial.childStamp==(childAssigned?stamp:0));
            require(Trace.saved==(childAssigned?value:null)&&Trace.stampSeen==(childAssigned?stamp:0));
            require(Trace.corePartial==(Trace.fail==0?partial:null)&&result==(Trace.fail==0?partial:null));
            if(value!=null)require(value.payload==payload);
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",events="+Trace.events+",stamp="+Long.toHexString(parent.stamp));
        }
    }
}
