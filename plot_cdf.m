function plot_cdf(a,Tlim,Tmax)
% This function plots the CDF of the RTT and 
% queue delay from the logs given by the 
% SCReAM BW test tool.
% Parameters :
%  a        : log file from SCReAM BW test tool
%             imported with the command
%             a = load(<logfile>);
%             where <logfile> is the name of the log file      
%  Tlim     : xmin and xmax limits [s], e.g. [0 100]
%  Tmax     : Max displayed value for RTT/delay
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
ix = intersect(find(T > Tlim(1)),find(T <= Tlim(2)));

qd = sort(a(ix,2));
rtt = sort(a(ix,3));
N = length(qd); N = (0:N-1)/(N-1);

subplot(111);
plot(qd,N,rtt,N);
set(gca,'FontSize',14);grid on;
title('CDF Network queue delay and RTT [s]');
legend('Queue delay','RTT');
xlim([0 Tmax]);

xlabel('[s]');
end

