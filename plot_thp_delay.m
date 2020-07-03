function plot_thp_delay(a,Tlim,maxThp,maxDelay)
% This function plots the thorughput 
% RTT and estimated queue delay
% Parameters:
%  a        : log file from SCReAM BW test tool
%             imported with the command
%             a = load(<logfile>);
%             where <logfile> is the name of the log file      
%  Tlim     : xmin and xmax limits [s], e.g. [0 100]
%  maxThp   : Max thorughput [Mbps]
%  maxDelay : Max delay [s]
%
% The script can be used with matlab or octave
% Octave can sometimes be painfully slow. The following commands
%  can make it faster
% >a = a = load(<logfile>);
% >a = a(1:50:end,:); % subsample the log file
% >figure(1);
% >plot_cdf(....
% 
T = a(:,1);
subplot(211);%subplot(9,1,1:5);
K = 5;
B = ones(1,K)/K;
ix = find(a(:,15) > 0);
plr = sum(a(:,15))/sum(a(:,10))*100
ixe = find(a(:,16) > 0);

mean(a(:,4))
plot(T,filter(B,1,a(:,13))/1e6,T(ix),0.5,'r.',T(ixe),0.5,'m.','linewidth',2)
set(gca,'FontSize',14);grid on;
set(gca,'XTickLabel',[]);grid on;
title('Throughput [Mbps], loss events (red)');
xlim(Tlim);
ylim([0 maxThp]);
subplot(212);%subplot(9,1,7:9);
plot(T,a(:,2),T,a(:,3),':','linewidth',2);ylim([0 maxDelay]);
set(gca,'FontSize',14);grid on;
title('Network queue delay (blue) and RTT (green) [s]');
%legend('Network','RTT');
xlim(Tlim);

xlabel('T [s]');
xlim(Tlim);
end

